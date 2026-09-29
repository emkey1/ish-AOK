//
//  LLMChatMCPSettings.m
//  iSH-AOK
//
//  LLM Settings -> MCP Servers: the servers whose tools the chat offers the
//  model. Add a remote (Streamable HTTP) server or one that runs in the
//  guest over stdio; turn each on or off, edit, check, delete. See
//  LLMChatMCP.h.
//

#import "LLMChatInternal.h"
#import "LLMChatMCP.h"
#import "UIViewController+Extras.h"

typedef NS_ENUM(NSInteger, ISHLLMMCPSection) {
    ISHLLMMCPSectionServers,
    ISHLLMMCPSectionAdd,
    ISHLLMMCPSectionCount,
};

static BOOL ISHLLMMCPIsGuest(NSDictionary *server) {
    return [server[@"kind"] isEqual:kISHLLMMCPKindGuest];
}

@implementation LLMMCPServersViewController

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"MCP Servers";
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self.tableView reloadData];
}

- (void)saveServers:(NSArray<NSDictionary *> *)servers {
    ISHLLMSetMCPServers(servers);
    // The next message reconnects with the new settings.
    [ISHLLMMCPManager.shared disconnectAll];
    [self.tableView reloadData];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void) tableView;
    return ISHLLMMCPSectionCount;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void) tableView;
    if (section == ISHLLMMCPSectionServers)
        return (NSInteger) ISHLLMMCPServers().count;
    if (section == ISHLLMMCPSectionAdd)
        return 2;
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    if (section == ISHLLMMCPSectionServers)
        return ISHLLMMCPServers().count == 0
            ? @"No servers yet. A server's tools are offered to the model alongside the built-in ones, and each call asks first unless MCP Tools is set to Allow in Tool Permissions."
            : @"Tap a server to check, edit, turn off or delete it. Its tools are offered to the model alongside the built-in ones; each call follows the MCP Tools setting in Tool Permissions.";
    if (section == ISHLLMMCPSectionAdd)
        return @"A remote server is a Streamable HTTP endpoint (https://…/mcp); its token is kept in the Keychain. A guest server is a command run inside iSH-AOK as the chat's tool account, speaking MCP on its stdin and stdout -- for example \"npx -y @modelcontextprotocol/server-filesystem /root\" or \"python3 server.py\". It needs that program installed in the guest, and it is started with the first message that uses tools.";
    return nil;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    if (indexPath.section == ISHLLMMCPSectionServers) {
        NSDictionary *server = ISHLLMMCPServers()[(NSUInteger) indexPath.row];
        BOOL enabled = [server[@"enabled"] boolValue];
        cell.textLabel.text = server[@"name"];
        NSString *target = ISHLLMMCPIsGuest(server) ? server[@"command"] : server[@"url"];
        cell.detailTextLabel.text = [NSString stringWithFormat:@"%@%@ · %@", ISHLLMMCPIsGuest(server) ? @"Guest" : @"Remote",
                                     enabled ? @"" : @" (off)", target ?: @""];
        cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
        cell.textLabel.textColor = enabled ? UIColor.labelColor : UIColor.secondaryLabelColor;
        cell.accessoryType = enabled ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    } else {
        cell.textLabel.text = indexPath.row == 0 ? @"Add Remote Server…" : @"Add Guest Server…";
        cell.textLabel.textColor = self.view.tintColor;
    }
    ISHWorkspaceScaleTableViewCell(cell, ISHWorkspaceTextScaleForViewController(self));
    return cell;
}

- (void)workspaceTextScaleDidChange {
    ISHWorkspaceRescaleTableView(self.tableView, ISHWorkspaceTextScaleForViewController(self));
}

- (BOOL)tableView:(UITableView *)tableView canEditRowAtIndexPath:(NSIndexPath *)indexPath {
    (void) tableView;
    return indexPath.section == ISHLLMMCPSectionServers;
}

- (void)tableView:(UITableView *)tableView commitEditingStyle:(UITableViewCellEditingStyle)editingStyle forRowAtIndexPath:(NSIndexPath *)indexPath {
    (void) tableView;
    if (editingStyle != UITableViewCellEditingStyleDelete || indexPath.section != ISHLLMMCPSectionServers)
        return;
    NSMutableArray *servers = [ISHLLMMCPServers() mutableCopy];
    if ((NSUInteger) indexPath.row < servers.count) {
        [servers removeObjectAtIndex:(NSUInteger) indexPath.row];
        [self saveServers:servers];
    }
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
    if (indexPath.section == ISHLLMMCPSectionAdd) {
        [self editServerAtIndex:NSNotFound kind:indexPath.row == 0 ? kISHLLMMCPKindRemote : kISHLLMMCPKindGuest];
        return;
    }
    NSUInteger index = (NSUInteger) indexPath.row;
    NSDictionary *server = ISHLLMMCPServers()[index];
    BOOL enabled = [server[@"enabled"] boolValue];
    ISHActionSheet *sheet = [ISHActionSheet actionSheetWithTitle:server[@"name"] message:nil];
    [sheet addActionWithTitle:@"Check" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [self checkServer:server];
    }];
    [sheet addActionWithTitle:@"Edit…" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [self editServerAtIndex:index kind:server[@"kind"]];
    }];
    [sheet addActionWithTitle:enabled ? @"Turn Off" : @"Turn On" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSMutableArray *servers = [ISHLLMMCPServers() mutableCopy];
        if (index >= servers.count)
            return;
        NSMutableDictionary *changed = [servers[index] mutableCopy];
        changed[@"enabled"] = @(!enabled);
        servers[index] = changed;
        [self saveServers:servers];
    }];
    [sheet addActionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *action) {
        NSMutableArray *servers = [ISHLLMMCPServers() mutableCopy];
        if (index < servers.count) {
            [servers removeObjectAtIndex:index];
            [self saveServers:servers];
        }
    }];
    [sheet addActionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil];
    [sheet presentFromViewController:self sourceView:cell sourceRect:cell.bounds];
}

- (void)checkServer:(NSDictionary *)server {
    UIAlertController *progress = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"Checking %@…", server[@"name"]]
        message:ISHLLMMCPIsGuest(server) ? @"Starting the server in the guest. The first start of an npx or uvx server downloads it, which can take a minute." : nil
        preferredStyle:UIAlertControllerStyleAlert];
    [self presentViewController:progress animated:YES completion:nil];
    [ISHLLMMCPManager.shared checkServer:server completion:^(NSArray<NSString *> *toolNames, NSString *error) {
        NSString *title = toolNames != nil ? @"Connected" : @"Could not connect";
        NSString *message = toolNames != nil
            ? (toolNames.count == 0 ? @"The server offers no tools." :
               [NSString stringWithFormat:@"%lu tool%@: %@", (unsigned long) toolNames.count, toolNames.count == 1 ? @"" : @"s", [toolNames componentsJoinedByString:@", "]])
            : error;
        [progress dismissViewControllerAnimated:YES completion:^{
            UIAlertController *result = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
            [result addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
            [self presentViewController:result animated:YES completion:nil];
        }];
    }];
}

// One alert for the whole server: name, then the URL and token or the
// command. index is NSNotFound for a new server.
- (void)editServerAtIndex:(NSUInteger)index kind:(NSString *)kind {
    NSArray<NSDictionary *> *servers = ISHLLMMCPServers();
    NSDictionary *existing = index < servers.count ? servers[index] : nil;
    BOOL guest = [kind isEqual:kISHLLMMCPKindGuest];
    NSString *title = existing != nil ? @"Edit MCP Server" : (guest ? @"Add Guest Server" : @"Add Remote Server");
    NSString *message = guest
        ? @"The command runs in the guest through the tool account's shell."
        : @"The server's Streamable HTTP URL. The token, if any, is sent as a bearer token.";
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    NSString *existingToken = existing != nil ? ISHLLMMCPServerToken(existing[@"id"]) : nil;
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.placeholder = @"Name";
        textField.text = existing[@"name"];
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
    }];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.placeholder = guest ? @"npx -y @modelcontextprotocol/server-…" : @"https://example.com/mcp";
        textField.text = guest ? existing[@"command"] : existing[@"url"];
        textField.keyboardType = guest ? UIKeyboardTypeDefault : UIKeyboardTypeURL;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.spellCheckingType = UITextSpellCheckingTypeNo;
    }];
    if (!guest) {
        [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
            textField.placeholder = @"Token (optional)";
            textField.text = existingToken;
            textField.secureTextEntry = YES;
            // Beside a name and a URL, a secure field reads to iOS as a login
            // form and it offers to save a "password" on Save.
            textField.textContentType = UITextContentTypeOneTimeCode;
            textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
            textField.autocorrectionType = UITextAutocorrectionTypeNo;
        }];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSCharacterSet *space = NSCharacterSet.whitespaceAndNewlineCharacterSet;
        NSString *name = [alert.textFields[0].text stringByTrimmingCharactersInSet:space];
        NSString *target = [alert.textFields[1].text stringByTrimmingCharactersInSet:space];
        if (target.length == 0)
            return;
        if (!guest) {
            NSURL *url = [NSURL URLWithString:target];
            if (url.host.length == 0 || !([url.scheme isEqualToString:@"https"] || [url.scheme isEqualToString:@"http"])) {
                UIAlertController *bad = [UIAlertController alertControllerWithTitle:@"Not a server URL"
                    message:@"Enter an http:// or https:// URL." preferredStyle:UIAlertControllerStyleAlert];
                [bad addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
                [self presentViewController:bad animated:YES completion:nil];
                return;
            }
        }
        if (name.length == 0)
            name = guest ? [target componentsSeparatedByString:@" "].firstObject : ([NSURL URLWithString:target].host ?: @"server");
        NSMutableDictionary *server = existing != nil ? [existing mutableCopy]
            : [@{@"id": NSUUID.UUID.UUIDString, @"kind": kind, @"enabled": @YES} mutableCopy];
        server[@"name"] = name;
        if (guest)
            server[@"command"] = target;
        else
            server[@"url"] = target;
        if (!guest) {
            NSString *token = [alert.textFields[2].text stringByTrimmingCharactersInSet:space];
            ISHLLMSetMCPServerToken(server[@"id"], token.length > 0 ? token : nil);
        }
        NSMutableArray *updated = [ISHLLMMCPServers() mutableCopy];
        if (existing != nil && index < updated.count)
            updated[index] = server;
        else
            [updated addObject:server];
        [self saveServers:updated];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

@end
