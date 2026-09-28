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
    ISHLLMSettingsRowDestinations,
    ISHLLMSettingsRowProvider,
    ISHLLMSettingsRowServerURL,
    ISHLLMSettingsRowModel,
    ISHLLMSettingsRowAPIFormat,
    ISHLLMSettingsRowAPIKey,
    ISHLLMSettingsRowQueryModels,
    ISHLLMSettingsRowTestConnection,
    ISHLLMSettingsRowShellTools,
    ISHLLMSettingsRowToolPermissions,
    ISHLLMSettingsRowCommandTimeout,
    ISHLLMSettingsRowOutputLimit,
    ISHLLMSettingsRowToolRounds,
    ISHLLMSettingsRowHideThinking,
    ISHLLMSettingsRowCount,
};

@implementation LLMSettingsViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"LLM Client";
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                                                           target:self
                                                                                           action:@selector(done:)];
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

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    if (section == 0)
        return 1;
    return ISHLLMSettingsRowCount;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    if (section == 0)
        return nil;
    NSString *thinkingNote = @"Hide Thinking collapses a reasoning model's <think> blocks behind a “Thinking” line in the transcript; tap it to expand or copy the reasoning. The full text is always kept in the saved history.";
    NSString *destinationsNote = @"Destinations are the saved endpoints the chat can switch between from its own toolbar; the rows below configure whichever one is selected. Chats are saved in /AOK/persist/llm-chats.";
    if (ISHLLMUsesAppleFoundationModels())
        return [NSString stringWithFormat:@"Apple Foundation Models is an iOS/iPadOS 26+ on-device backend; no server URL or API key needed. %@ Tools lets it run commands in the iSH-AOK shell, as Tool Permissions allows; the command timeout, output limit, and tool call round cap are adjustable above. %@ %@", ISHLLMAppleFoundationModelsUnavailableMessage(), thinkingNote, destinationsNote];
    return [NSString stringWithFormat:@"Use a /v1 OpenAI-compatible server, or the Gemini preset. Hosted providers require API keys.\nTools lets an OpenAI-compatible model read, search and edit files and run commands in the iSH-AOK shell, as Tool Permissions allows; not available for Gemini. The command timeout, output limit, and tool call round cap are adjustable above.\n%@\n%@", thinkingNote, destinationsNote];
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
        cell.textLabel.text = @"Back to Chat";
        cell.detailTextLabel.text = @"Done";
        cell.accessoryType = UITableViewCellAccessoryNone;
        return cell;
    }
    BOOL onDevice = ISHLLMUsesAppleFoundationModels();
    switch ((ISHLLMSettingsRow) indexPath.row) {
        case ISHLLMSettingsRowDestinations: {
            // Name the ACTIVE one, not just how many there are. A bare count
            // reads as bookkeeping; the name reads as "this is what you are
            // talking to, and this row is where you change it".
            NSUInteger count = ISHLLMDestinations().count;
            NSString *active = ISHLLMDestinationDisplayName(ISHLLMActiveDestination());
            cell.textLabel.text = @"Destinations";
            cell.detailTextLabel.text = count == 1
                ? active
                : [NSString stringWithFormat:@"%@ · %lu saved", active, (unsigned long) count];
            break;
        }
        case ISHLLMSettingsRowProvider:
            cell.textLabel.text = @"Provider";
            cell.detailTextLabel.text = UserPreferences.shared.llmProvider;
            break;
        case ISHLLMSettingsRowServerURL:
            cell.textLabel.text = @"Server URL";
            cell.detailTextLabel.text = onDevice ? @"On-device" : UserPreferences.shared.llmServerURL;
            if (onDevice)
                cell.accessoryType = UITableViewCellAccessoryNone;
            break;
        case ISHLLMSettingsRowModel:
            cell.textLabel.text = @"Model";
            cell.detailTextLabel.text = UserPreferences.shared.llmModel;
            break;
        case ISHLLMSettingsRowAPIFormat:
            cell.textLabel.text = @"API Format";
            cell.detailTextLabel.text = ISHLLMCurrentAPIFormat();
            cell.accessoryType = UITableViewCellAccessoryNone;
            break;
        case ISHLLMSettingsRowAPIKey:
            cell.textLabel.text = @"API Key";
            cell.detailTextLabel.text = onDevice ? @"Not used" : (UserPreferences.shared.llmAPIKey.length > 0 ? @"Set" : (ISHLLMProviderRequiresAPIKey() ? @"Required" : @"Optional"));
            if (onDevice)
                cell.accessoryType = UITableViewCellAccessoryNone;
            break;
        case ISHLLMSettingsRowQueryModels:
            cell.textLabel.text = @"Query Models";
            cell.detailTextLabel.text = @"/models";
            cell.accessoryType = UITableViewCellAccessoryNone;
            break;
        case ISHLLMSettingsRowTestConnection:
            cell.textLabel.text = @"Test Connection";
            cell.detailTextLabel.text = @"";
            cell.accessoryType = UITableViewCellAccessoryNone;
            break;
        case ISHLLMSettingsRowShellTools:
            cell.textLabel.text = @"Tools";
            cell.detailTextLabel.text = UserPreferences.shared.llmToolsEnabled ? @"On" : @"Off";
            cell.accessoryType = UserPreferences.shared.llmToolsEnabled ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
            break;
        case ISHLLMSettingsRowToolPermissions:
            cell.textLabel.text = @"Tool Permissions";
            cell.detailTextLabel.text = [NSString stringWithFormat:@"Edit %@ · Shell %@",
                ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryEdit)),
                ISHLLMPermissionActionTitle(ISHLLMCategoryAction(ISHLLMToolCategoryShell))];
            break;
        case ISHLLMSettingsRowCommandTimeout:
            cell.textLabel.text = @"Command Timeout";
            cell.detailTextLabel.text = ISHLLMToolTimeoutTitle(ISHLLMToolTimeoutSeconds());
            break;
        case ISHLLMSettingsRowOutputLimit:
            cell.textLabel.text = @"Output Limit";
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%ld KB", (long) ISHLLMToolOutputLimitKB()];
            break;
        case ISHLLMSettingsRowToolRounds:
            cell.textLabel.text = @"Tool Call Rounds";
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%ld", (long) ISHLLMToolMaxRounds()];
            break;
        case ISHLLMSettingsRowHideThinking:
            cell.textLabel.text = @"Hide Thinking";
            cell.detailTextLabel.text = UserPreferences.shared.llmHideThinking ? @"On" : @"Off";
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
    // The rows below edit the SELECTED destination; the Destinations row is
    // where a second one is added or a different one selected.
    ISHLLMSettingsRow row = (ISHLLMSettingsRow) indexPath.row;
    if (ISHLLMUsesAppleFoundationModels() && (row == ISHLLMSettingsRowServerURL || row == ISHLLMSettingsRowAPIKey))
        return;
    switch (row) {
        case ISHLLMSettingsRowDestinations: {
            LLMDestinationListViewController *destinations = [LLMDestinationListViewController new];
            __weak typeof(self) weakSelf = self;
            destinations.destinationsChanged = ^{ [weakSelf.tableView reloadData]; };
            [self.navigationController pushViewController:destinations animated:YES];
            return;
        }
        case ISHLLMSettingsRowProvider:
            [self.navigationController pushViewController:[LLMProviderPickerViewController new] animated:YES];
            return;
        case ISHLLMSettingsRowAPIFormat:
        case ISHLLMSettingsRowCount:
            return;
        case ISHLLMSettingsRowQueryModels:
            [self queryAvailableModelsFromView:cell];
            return;
        case ISHLLMSettingsRowTestConnection:
            [self testConnection];
            return;
        case ISHLLMSettingsRowShellTools:
            [self toggleShellToolsFromView:cell];
            return;
        case ISHLLMSettingsRowToolPermissions:
            [self.navigationController pushViewController:[LLMToolPermissionsViewController new] animated:YES];
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
        case ISHLLMSettingsRowHideThinking:
            UserPreferences.shared.llmHideThinking = !UserPreferences.shared.llmHideThinking;
            [tableView reloadData];
            return;
        case ISHLLMSettingsRowServerURL:
        case ISHLLMSettingsRowModel:
        case ISHLLMSettingsRowAPIKey:
            break; // the free-text rows, edited below
    }

    NSString *title = row == ISHLLMSettingsRowServerURL ? @"Server URL" : (row == ISHLLMSettingsRowModel ? @"Model" : @"API Key");
    NSString *current = row == ISHLLMSettingsRowServerURL ? UserPreferences.shared.llmServerURL : (row == ISHLLMSettingsRowModel ? UserPreferences.shared.llmModel : UserPreferences.shared.llmAPIKey);
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = current;
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.spellCheckingType = UITextSpellCheckingTypeNo;
        if (row == ISHLLMSettingsRowAPIKey)
            textField.secureTextEntry = YES;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSString *value = alert.textFields.firstObject.text ?: @"";
        if (row == ISHLLMSettingsRowServerURL)
            UserPreferences.shared.llmServerURL = value;
        else if (row == ISHLLMSettingsRowModel)
            UserPreferences.shared.llmModel = value;
        else
            UserPreferences.shared.llmAPIKey = value;
        // Keeps the saved destination describing the live configuration.
        ISHLLMSyncActiveDestinationFromPreferences();
        [self.tableView reloadData];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)toggleShellToolsFromView:(UIView *)sourceView {
    (void) sourceView;
    if (UserPreferences.shared.llmToolsEnabled) {
        UserPreferences.shared.llmToolsEnabled = NO;
        [self.tableView reloadData];
        return;
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Enable tools?"
        message:[NSString stringWithFormat:@"The model will be able to read, search and change files and run shell commands in the iSH-AOK Linux environment — to work on code, fetch web pages with curl/wget, or run programs. By default it reads files freely and asks you before each edit or command; Tool Permissions changes that. Output is capped at %ld KB and commands are killed after %@ (both adjustable below). Only enable this with a model and server you trust.",
            (long) ISHLLMToolOutputLimitKB(), ISHLLMToolTimeoutTitle(ISHLLMToolTimeoutSeconds())]
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Enable" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        UserPreferences.shared.llmToolsEnabled = YES;
        [self.tableView reloadData];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

// Preset pickers for the shell-tool limits. Action sheets need a popover anchor
// on iPad, so both take the tapped cell as the source view.
- (void)pickToolTimeoutFromView:(UIView *)sourceView {
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:@"Command Timeout"
        message:@"A command that runs longer than this is killed and its partial output is returned to the model."];
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
    [sheet addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:sourceView sourceRect:sourceView.bounds];
}

- (void)pickToolOutputLimitFromView:(UIView *)sourceView {
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:@"Output Limit"
        message:@"Command output beyond this is truncated before being returned to the model. Larger limits use more of the model's context window."];
    NSInteger current = ISHLLMToolOutputLimitKB();
    for (NSNumber *choice in @[@16, @64, @128, @256]) {
        NSInteger kb = choice.integerValue;
        NSString *title = [NSString stringWithFormat:@"%ld KB%@", (long) kb, kb == current ? @" ✓" : @""];
        [sheet addActionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmToolOutputLimitKB = kb;
            [self.tableView reloadData];
        }];
    }
    [sheet addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:sourceView sourceRect:sourceView.bounds];
}

- (void)pickToolMaxRoundsFromView:(UIView *)sourceView {
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:@"Tool Call Rounds"
        message:@"A model reply that keeps calling tools without giving a final answer is stopped after this many rounds in a row, so a stuck model can't loop forever. Each round is one request to the model, so higher values let longer multi-step tasks (installing something, then using it) finish without you having to nudge it to continue."];
    NSInteger current = ISHLLMToolMaxRounds();
    for (NSNumber *choice in @[@6, @10, @15, @20, @30, @50]) {
        NSInteger rounds = choice.integerValue;
        NSString *title = [NSString stringWithFormat:@"%ld%@", (long) rounds, rounds == current ? @" ✓" : @""];
        [sheet addActionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmToolMaxRounds = rounds;
            [self.tableView reloadData];
        }];
    }
    [sheet addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:sourceView sourceRect:sourceView.bounds];
}

- (NSArray<NSString *> *)modelIdentifiersFromResponseData:(NSData *)data {
    return ISHLLMModelIdentifiersFromResponseData(data);
}

- (void)presentModelPickerWithModels:(NSArray<NSString *> *)models statusCode:(NSInteger)statusCode error:(NSError *)error fromView:(UIView *)sourceView {
    if (error != nil) {
        [self showConnectionResult:error.localizedDescription title:@"Model Query Failed"];
        return;
    }
    if (models.count == 0) {
        NSString *message = statusCode > 0 ? [NSString stringWithFormat:@"No models found. HTTP %ld", (long) statusCode] : @"No models found.";
        [self showConnectionResult:message title:@"Model Query Failed"];
        return;
    }
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:@"Choose Model"
                                                         message:[NSString stringWithFormat:@"%lu models returned by %@", (unsigned long) models.count, ISHLLMModelsEndpoint()]];
    NSUInteger limit = MIN(models.count, 80);
    for (NSUInteger i = 0; i < limit; i++) {
        NSString *model = models[i];
        NSString *title = [model isEqualToString:UserPreferences.shared.llmModel] ? [model stringByAppendingString:@"  Current"] : model;
        [alert addActionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmModel = model;
            ISHLLMSyncActiveDestinationFromPreferences();
            [self.tableView reloadData];
        }];
    }
    if (models.count > limit) {
        [alert addActionWithTitle:[NSString stringWithFormat:@"Showing first %lu of %lu", (unsigned long) limit, (unsigned long) models.count]
                            style:UIAlertActionStyleDefault
                          handler:nil];
    }
    [alert addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [alert presentFromViewController:self source:sourceView];
}

- (void)queryAvailableModelsFromView:(UIView *)sourceView {
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        [self showConnectionResult:[NSString stringWithFormat:@"Current on-device model: %@\n%@", UserPreferences.shared.llmModel.length > 0 ? UserPreferences.shared.llmModel : @"system-language-model", ISHLLMAppleFoundationModelsUnavailableMessage()] title:@"Apple Foundation Models"];
        return;
    }
    NSURL *url = [NSURL URLWithString:ISHLLMModelsEndpoint()];
    if (url == nil) {
        [self showConnectionResult:@"Invalid models URL." title:@"Model Query Failed"];
        return;
    }
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    if ([[url.scheme lowercaseString] isEqualToString:@"http"]) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSInteger statusCode = 0;
            NSError *error = nil;
            NSData *data = ISHLLMDirectHTTPGet(url, apiKey, &statusCode, &error);
            NSArray<NSString *> *models = [self modelIdentifiersFromResponseData:data];
            dispatch_async(dispatch_get_main_queue(), ^{
                [self presentModelPickerWithModels:models statusCode:statusCode error:error fromView:sourceView];
            });
        });
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    if (apiKey.length > 0 && !ISHLLMUsesGeminiAPI())
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
    NSURLSessionDataTask *task = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSArray<NSString *> *models = [self modelIdentifiersFromResponseData:data];
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self presentModelPickerWithModels:models statusCode:http.statusCode error:error fromView:sourceView];
        });
    }];
    [task resume];
}

- (void)testConnection {
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        [self showConnectionResult:ISHLLMAppleFoundationModelsUnavailableMessage() title:@"Apple Foundation Models"];
        return;
    }
    NSString *model = [UserPreferences.shared.llmModel stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSURL *url = [NSURL URLWithString:ISHLLMUsesGeminiAPI() ? ISHLLMGeminiGenerateEndpoint() : ISHLLMChatEndpoint()];
    if (model.length == 0 || url == nil) {
        [self showConnectionResult:@"Set a valid server URL and model first." title:@"LLM Test Failed"];
        return;
    }
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    if (ISHLLMProviderRequiresAPIKey() && apiKey.length == 0) {
        [self showConnectionResult:ISHLLMMissingAPIKeyMessage() title:@"LLM Test Failed"];
        return;
    }
    NSDictionary *body = ISHLLMUsesGeminiAPI()
        ? @{@"contents": @[@{@"role": @"user", @"parts": @[@{@"text": @"Reply with exactly: ok"}]}]}
        : @{
            @"model": model,
            @"messages": @[@{@"role": @"user", @"content": @"Reply with exactly: ok"}],
            @"stream": @NO,
            @"max_tokens": @8,
        };
    NSData *bodyData = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    if ([[url.scheme lowercaseString] isEqualToString:@"http"]) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSInteger statusCode = 0;
            NSError *error = nil;
            NSData *data = ISHLLMDirectHTTPPost(url, bodyData, apiKey, &statusCode, &error);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (error != nil) {
                    [self showConnectionResult:error.localizedDescription title:@"LLM Test Failed"];
                } else {
                    NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
                    NSString *message = [NSString stringWithFormat:@"HTTP %ld\n%@", (long) statusCode, raw.length > 240 ? [raw substringToIndex:240] : raw];
                    [self showConnectionResult:message title:(statusCode >= 200 && statusCode < 300 ? @"LLM Test OK" : @"LLM Test Failed")];
                }
            });
        });
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    if (apiKey.length > 0 && !ISHLLMUsesGeminiAPI())
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
    request.HTTPBody = bodyData;
    NSURLSessionDataTask *task = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (error != nil) {
                [self showConnectionResult:error.localizedDescription title:@"LLM Test Failed"];
            } else {
                NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
                NSString *raw = data.length > 0 ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"";
                NSString *message = [NSString stringWithFormat:@"HTTP %ld\n%@", (long) http.statusCode, raw.length > 240 ? [raw substringToIndex:240] : raw];
                [self showConnectionResult:message title:(http.statusCode >= 200 && http.statusCode < 300 ? @"LLM Test OK" : @"LLM Test Failed")];
            }
        });
    }];
    [task resume];
}

- (void)showConnectionResult:(NSString *)message title:(NSString *)title {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end

@implementation LLMProviderPickerViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"LLM Provider";
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    (void) section;
    return ISHLLMProviderPresets().count;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    (void) section;
    return @"Choose a provider preset. Custom values can still be edited afterward.";
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
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    NSString *name = ISHLLMProviderPresets()[indexPath.row][@"name"];
    cell.textLabel.text = name;
    cell.accessoryType = [name isEqualToString:UserPreferences.shared.llmProvider]
        ? UITableViewCellAccessoryCheckmark
        : UITableViewCellAccessoryNone;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSDictionary<NSString *, NSString *> *preset = ISHLLMProviderPresets()[indexPath.row];
    NSString *name = preset[@"name"];

    // Picking a provider SWITCHES destinations; it does not overwrite one.
    //
    // This used to write the four scalars and then sync them into the active
    // destination, so choosing Groq while OpenAI was active replaced the OpenAI
    // entry -- its model, its URL, and the fact it existed at all. Anyone who
    // uses two providers had to know to go to Destinations and add one FIRST,
    // and if they did not, the only copy of the old setup was gone. It also
    // carried the previous provider's API key over to the new one, which is
    // both wrong and quietly confusing to debug.
    //
    // So: if a saved destination already uses this provider, activate it and
    // restore its model, URL and key. Otherwise add a new one seeded from the
    // preset and leave the current destination untouched.
    for (NSDictionary<NSString *, NSString *> *destination in ISHLLMDestinations()) {
        if ([ISHLLMStringValue(destination, kISHLLMDestinationProvider) isEqualToString:name]) {
            ISHLLMActivateDestination(destination);
            [self.navigationController popViewControllerAnimated:YES];
            return;
        }
    }

    NSDictionary<NSString *, NSString *> *fresh = @{
        kISHLLMDestinationID: NSUUID.UUID.UUIDString,
        kISHLLMDestinationName: name,
        kISHLLMDestinationProvider: name,
        kISHLLMDestinationURL: preset[@"url"] ?: @"",
        kISHLLMDestinationModel: preset[@"model"] ?: @"",
        // Deliberately empty: a key belongs to the provider that issued it.
        kISHLLMDestinationAPIKey: @"",
    };
    ISHLLMSaveDestination(fresh);
    ISHLLMActivateDestination(fresh);
    [self.navigationController popViewControllerAnimated:YES];
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
    NSRelativeDateTimeFormatter *formatter = [NSRelativeDateTimeFormatter new];
    formatter.unitsStyle = NSRelativeDateTimeFormatterUnitsStyleShort;
    return [formatter localizedStringForDate:[NSDate dateWithTimeIntervalSince1970:timestamp] relativeToDate:NSDate.date];
}

@implementation LLMChatSessionListViewController {
    NSArray<NSDictionary<NSString *, id> *> *_sessions;
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Chats";
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                                                          target:self
                                                                                          action:@selector(done:)];
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCompose
                                                                                          target:self
                                                                                          action:@selector(newChat:)];
    [self reload];
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
    return @"Each chat keeps its own history, destination and system prompt. Swipe a chat to rename or delete it. Chats are saved in /AOK/persist/llm-chats.";
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    NSDictionary<NSString *, id> *entry = _sessions[indexPath.row];
    NSString *title = ISHLLMStringValue(entry, @"title");
    cell.textLabel.text = title.length > 0 ? title : @"New Chat";

    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    NSInteger count = [entry[@"count"] isKindOfClass:NSNumber.class] ? [entry[@"count"] integerValue] : 0;
    [parts addObject:count == 1 ? @"1 message" : [NSString stringWithFormat:@"%ld messages", (long) count]];
    NSString *when = ISHLLMRelativeDateDescription([entry[@"updated"] isKindOfClass:NSNumber.class] ? [entry[@"updated"] doubleValue] : 0.0);
    if (when.length > 0)
        [parts addObject:when];
    NSString *destination = ISHLLMDestinationNameForID(ISHLLMStringValue(entry, @"destination"));
    if (destination.length > 0)
        [parts addObject:destination];
    if (ISHLLMStringValue(entry, @"system").length > 0)
        [parts addObject:@"system prompt"];
    cell.detailTextLabel.text = [parts componentsJoinedByString:@" · "];
    cell.detailTextLabel.numberOfLines = 1;
    if (@available(iOS 13.0, *))
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
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
                                                                              title:@"Delete"
                                                                            handler:^(__unused UIContextualAction *action, __unused UIView *sourceView, void (^completion)(BOOL)) {
        NSString *nextSessionID = ISHLLMDeleteSession(sessionID);
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
                                                                              title:@"Rename"
                                                                            handler:^(__unused UIContextualAction *action, __unused UIView *sourceView, void (^completion)(BOOL)) {
        completion(YES);
        [self renameSessionWithID:sessionID currentTitle:ISHLLMStringValue(entry, @"title")];
    }];
    return [UISwipeActionsConfiguration configurationWithActions:@[deleteAction, renameAction]];
}

- (void)renameSessionWithID:(NSString *)sessionID currentTitle:(NSString *)currentTitle {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Rename Chat" message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = currentTitle;
        textField.placeholder = @"Chat name";
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Rename" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
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
@interface LLMDestinationEditorViewController : UITableViewController <WorkspaceTextScaledPage>
@property (nonatomic, copy) NSDictionary<NSString *, NSString *> *destination;
@property (nonatomic, copy) void (^destinationSaved)(void);
@end

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
    self.title = @"Destination";
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    (void) section;
    return ISHLLMDestinationEditorRowCount;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    (void) section;
    return @"A preset fills in the provider, server URL and model; each stays editable. The API key is stored with this destination, in app preferences, the same place the single-endpoint key was always kept.";
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
    switch ((ISHLLMDestinationEditorRow) indexPath.row) {
        case ISHLLMDestinationEditorRowName:
            cell.textLabel.text = @"Name";
            cell.detailTextLabel.text = ISHLLMDestinationDisplayName(self.destination);
            break;
        case ISHLLMDestinationEditorRowPreset:
            cell.textLabel.text = @"Provider";
            cell.detailTextLabel.text = ISHLLMStringValue(self.destination, kISHLLMDestinationProvider);
            break;
        case ISHLLMDestinationEditorRowServerURL:
            cell.textLabel.text = @"Server URL";
            cell.detailTextLabel.text = ISHLLMStringValue(self.destination, kISHLLMDestinationURL);
            break;
        case ISHLLMDestinationEditorRowModel:
            cell.textLabel.text = @"Model";
            cell.detailTextLabel.text = ISHLLMStringValue(self.destination, kISHLLMDestinationModel);
            break;
        case ISHLLMDestinationEditorRowAPIKey:
            cell.textLabel.text = @"API Key";
            cell.detailTextLabel.text = ISHLLMStringValue(self.destination, kISHLLMDestinationAPIKey).length > 0 ? @"Set" : @"Not set";
            break;
        case ISHLLMDestinationEditorRowCount:
            break;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.row == ISHLLMDestinationEditorRowPreset) {
        [self pickPresetFromView:[tableView cellForRowAtIndexPath:indexPath]];
        return;
    }
    NSString *field = nil;
    NSString *title = nil;
    switch ((ISHLLMDestinationEditorRow) indexPath.row) {
        case ISHLLMDestinationEditorRowName: field = kISHLLMDestinationName; title = @"Name"; break;
        case ISHLLMDestinationEditorRowServerURL: field = kISHLLMDestinationURL; title = @"Server URL"; break;
        case ISHLLMDestinationEditorRowModel: field = kISHLLMDestinationModel; title = @"Model"; break;
        case ISHLLMDestinationEditorRowAPIKey: field = kISHLLMDestinationAPIKey; title = @"API Key"; break;
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
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSMutableDictionary<NSString *, NSString *> *updated = [self.destination mutableCopy];
        updated[field] = alert.textFields.firstObject.text ?: @"";
        [self commitDestination:updated];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)pickPresetFromView:(UIView *)sourceView {
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:@"Provider" message:@"Fills in the server URL and a default model."];
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
    [alert addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [alert presentFromViewController:self source:sourceView];
}

- (void)commitDestination:(NSDictionary<NSString *, NSString *> *)destination {
    self.destination = destination;
    ISHLLMSaveDestination(destination);
    [self.tableView reloadData];
    if (self.destinationSaved != nil)
        self.destinationSaved();
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
    self.title = @"Destinations";
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
    ISHActionSheet *alert = [ISHActionSheet actionSheetWithTitle:@"Add Destination" message:@"Start from a provider preset."];
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
    [alert addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
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
    return @"Tap a destination to chat with it; tap the arrow to edit it. The selected destination is what the chat, Test Connection and Query Models all use. Swipe to duplicate or delete.";
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
                                                                              title:@"Delete"
                                                                            handler:^(__unused UIContextualAction *action, __unused UIView *sourceView, void (^completion)(BOOL)) {
        BOOL deleted = ISHLLMDeleteDestinationWithID(destinationID);
        completion(deleted);
        if (!deleted) {
            [self presentMessage:@"The last destination can't be deleted. Edit it, or add another one first."];
            return;
        }
        [self reload];
        [self notifyChanged];
    }];
    // Duplicating is the cheap way to have the same server twice with two
    // models, which is the common multi-destination setup for a local server.
    UIContextualAction *duplicateAction = [UIContextualAction contextualActionWithStyle:UIContextualActionStyleNormal
                                                                                 title:@"Duplicate"
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
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
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
    self.title = @"Tool Permissions";
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
        case ISHLLMPermissionsSectionCategories: return 3;
        case ISHLLMPermissionsSectionRules: return (NSInteger) ISHLLMShellRules().count + 1;
        case ISHLLMPermissionsSectionActions: return 1;
        case ISHLLMPermissionsSectionCount: break;
    }
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void) tableView;
    if (section == ISHLLMPermissionsSectionCategories)
        return @"When the model wants to";
    if (section == ISHLLMPermissionsSectionRules)
        return @"Shell command rules";
    return nil;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    if (section == ISHLLMPermissionsSectionCategories)
        return @"Allow runs it without asking, Ask shows you each one first, Deny refuses it and tells the model so. File edits outside the chat's working directory always ask unless edits are denied.";
    if (section == ISHLLMPermissionsSectionRules)
        return @"Each part of a command line (split at ; & | && || and newlines) takes the first rule its text matches, or the Shell Commands setting when none does; the strictest part decides. * matches anything, so \"git status *\" also matches a bare \"git status\". A rule cannot allow a part that writes to a file with > or a line with $( ) or backquotes: those get the Shell Commands setting. Drag to reorder in Edit mode.";
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
                cell.textLabel.text = @"Add Rule…";
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
            cell.textLabel.text = @"Restore Default Rules";
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
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Restore the default rules?"
                message:@"Your shell command rules are replaced by the defaults, which allow only commands that look at things (ls, cat, git status, ...)."
                preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
            [alert addAction:[UIAlertAction actionWithTitle:@"Restore" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
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
    [sheet addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:sourceView sourceRect:sourceView.bounds];
}

// Pattern first, then what it does: one alert with a text field, then the
// same Allow/Ask/Deny sheet the categories use.
- (void)editRuleAtIndex:(NSUInteger)index existing:(NSDictionary<NSString *, id> *)existing sourceView:(UIView *)sourceView {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:existing != nil ? @"Edit Rule" : @"Add Rule"
        message:@"A command pattern, e.g. \"make *\" or \"rm *\"."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = existing[kISHLLMShellRulePattern];
        textField.placeholder = @"make *";
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.spellCheckingType = UITextSpellCheckingTypeNo;
        textField.font = [UIFont monospacedSystemFontOfSize:UIFont.labelFontSize weight:UIFontWeightRegular];
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Next" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
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
