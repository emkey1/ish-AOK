//
//  AboutViewController.m
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import "AboutViewController.h"
#import "AppDelegate.h"
#import "CurrentRoot.h"
#import "AppGroup.h"
#import "Diagnostics.h"
#import "UserPreferences.h"
#import "iOSFS.h"
#import "UIApplication+OpenURL.h"
#import "NSObject+SaneKVO.h"
#import "SceneDelegate.h"
#import "Terminal.h"
#import "UIViewController+Extras.h"
#import "WorkspaceViewController.h"
#import "MarkdownRenderer.h"
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

// Connect to one of the resolved addresses with a bounded timeout (default
// blocking connect can hang ~75s on an unreachable host). Returns a connected,
// blocking socket fd, or -1 with *errnoOut set to the last failure reason.
static int ISHLLMConnectWithTimeout(struct addrinfo *results, int timeoutMs, int *errnoOut) {
    int lastErrno = ETIMEDOUT;
    for (struct addrinfo *addr = results; addr != NULL; addr = addr->ai_next) {
        int fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0) { lastErrno = errno; continue; }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0) {
            fcntl(fd, F_SETFL, flags);
            return fd;
        }
        if (errno != EINPROGRESS) { lastErrno = errno; close(fd); continue; }
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int pr = poll(&pfd, 1, timeoutMs);
        if (pr == 0) { lastErrno = ETIMEDOUT; close(fd); continue; }
        if (pr < 0) { lastErrno = errno; close(fd); continue; }
        int soError = 0;
        socklen_t soLen = sizeof(soError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &soLen) < 0 || soError != 0) {
            lastErrno = soError != 0 ? soError : errno;
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, flags);
        return fd;
    }
    if (errnoOut != NULL)
        *errnoOut = lastErrno;
    return -1;
}

// Build a user-facing NSError for a failed connection that names the host:port and
// the reason, so timeouts/refusals are obvious instead of a bare errno.
static NSError *ISHLLMConnectionError(NSString *host, NSString *port, int errnoValue) {
    NSString *reason = [NSString stringWithUTF8String:strerror(errnoValue)] ?: @"connection failed";
    NSString *message = [NSString stringWithFormat:@"Could not connect to %@:%@ — %@. The model server must be reachable from this device (check the URL/port, that the server is listening on all interfaces, and any VPN/firewall between them).", host, port, reason];
    return [NSError errorWithDomain:NSPOSIXErrorDomain code:errnoValue userInfo:@{NSLocalizedDescriptionKey: message}];
}

// Dedicated serial queue for guest-shell tool execution. run_guest_command_capture
// runs emulated guest code (spawns a task, manipulates kernel signal/`current`
// state) and must be kept OFF the shared libdispatch global pool that the HTTP
// requests run on -- otherwise running a tool command can leave a pooled worker
// thread in a state that wedges later network requests (observed as connect()
// timeouts after a tool ran). A private queue gives guest commands their own,
// isolated worker thread.
static dispatch_queue_t ISHLLMGuestCommandQueue(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("aok.llm.guest-shell", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

NSString *const kPreferenceOpenDiagnosticsOnLaunchKey = @"openDiagnosticsOnLaunch";

UINavigationController *ISHCreateAboutNavigationController(BOOL recoveryMode, BOOL startInDiagnostics) {
    UINavigationController *navigationController = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
    AboutViewController *aboutViewController = (AboutViewController *) navigationController.topViewController;
    aboutViewController.recoveryMode = recoveryMode;
    aboutViewController.startInDiagnostics = startInDiagnostics;
    return navigationController;
}

@interface DiagnosticsViewController : UIViewController
@end

@interface LLMClientViewController : UIViewController <UITextViewDelegate, UITableViewDataSource, UITableViewDelegate>

@property (nonatomic, copy) NSString *initialPrompt;

@end

@interface LLMSettingsViewController : UITableViewController
@end

@interface LLMProviderPickerViewController : UITableViewController
@end

// The saved chats, newest first: switch, rename, delete. Presented modally
// (the chat is an embedded child view controller in Workspace mode, where a
// push has nowhere to go).
@interface LLMChatSessionListViewController : UITableViewController
@property (nonatomic, copy) NSString *currentSessionID;
@property (nonatomic, copy) void (^sessionSelected)(NSString *sessionID);
@end

// The saved destinations: select, edit, duplicate, delete, add from a preset.
// Reachable both from the chat's destination menu and from LLM Settings.
@interface LLMDestinationListViewController : UITableViewController
@property (nonatomic, copy) void (^destinationsChanged)(void);
@end

UIViewController *ISHCreateDiagnosticsViewController(void) {
    return [DiagnosticsViewController new];
}

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

@interface AboutViewController ()
@property (weak, nonatomic) IBOutlet UITableViewCell *capsLockMappingCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *themeCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *initialWindowCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *diagnosticsCell;
@property (weak, nonatomic) IBOutlet UISwitch *disableDimmingSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableMulticoreSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableHLESwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableCryptoAccelSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enablePixAccelSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableExtraLockingSwitch;
@property (weak, nonatomic) IBOutlet UITextField *launchCommandField;
@property (weak, nonatomic) IBOutlet UITextField *bootCommandField;

@property (weak, nonatomic) IBOutlet UITableViewCell *sendFeedback;
@property (weak, nonatomic) IBOutlet UITableViewCell *openGithub;
@property (weak, nonatomic) IBOutlet UITableViewCell *openDiscord;
@property (weak, nonatomic) IBOutlet UITableViewCell *customDnsCell;

@property (weak, nonatomic) IBOutlet UITableViewCell *upgradeApkCell;
@property (weak, nonatomic) IBOutlet UILabel *upgradeApkLabel;
@property (weak, nonatomic) IBOutlet UIView *upgradeApkBadge;
@property (weak, nonatomic) IBOutlet UITableViewCell *exportContainerCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *resetMountsCell;

@property (weak, nonatomic) IBOutlet UILabel *versionLabel;

@property (nonatomic, strong) UISwitch *llmClientSwitch;
@property (nonatomic, strong) UISwitch *loginAsDefaultUserSwitch;

@end

@implementation DiagnosticsViewController {
    UITextView *_textView;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Diagnostics";
    if (@available(iOS 13.0, *)) {
        self.view.backgroundColor = UIColor.systemBackgroundColor;
    } else {
        self.view.backgroundColor = UIColor.whiteColor;
    }

    _textView = [[UITextView alloc] initWithFrame:self.view.bounds];
    _textView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _textView.editable = NO;
    _textView.alwaysBounceVertical = YES;
    if (@available(iOS 13.0, *)) {
        _textView.backgroundColor = UIColor.systemBackgroundColor;
        _textView.textColor = UIColor.labelColor;
        _textView.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    } else {
        _textView.backgroundColor = UIColor.whiteColor;
        _textView.textColor = UIColor.blackColor;
        _textView.font = [UIFont fontWithName:@"Menlo-Regular" size:12] ?: [UIFont systemFontOfSize:12];
    }
    [self.view addSubview:_textView];

    self.navigationItem.rightBarButtonItems = @[
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAction
                                                      target:self
                                                      action:@selector(exportDiagnostics:)],
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
                                                      target:self
                                                      action:@selector(refreshDiagnostics:)],
    ];

    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(refreshDiagnostics:)
                                               name:ISHDiagnosticsStoreDidUpdateNotification
                                             object:nil];
    [self refreshDiagnostics:nil];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshDiagnostics:nil];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if ([NSUserDefaults.standardUserDefaults boolForKey:kPreferenceOpenDiagnosticsOnLaunchKey]) {
        [NSUserDefaults.standardUserDefaults setBool:NO forKey:kPreferenceOpenDiagnosticsOnLaunchKey];
    }
}

- (void)refreshDiagnostics:(id)sender {
    _textView.text = [ISHDiagnosticsStore diagnosticsReport];
    [_textView setContentOffset:CGPointZero animated:NO];
}

- (void)exportDiagnostics:(id)sender {
    NSError *error = nil;
    NSURL *bundleURL = [ISHDiagnosticsStore prepareExportBundle:&error];
    if (bundleURL == nil) {
        [self presentError:error title:@"Export failed"];
        return;
    }

    UIActivityViewController *activityViewController =
        [[UIActivityViewController alloc] initWithActivityItems:@[bundleURL] applicationActivities:nil];
    UIPopoverPresentationController *popover = activityViewController.popoverPresentationController;
    if (popover != nil) {
        popover.barButtonItem = sender;
    }
    [self presentViewController:activityViewController animated:YES completion:nil];
}

@end

static NSURL *ISHLLMPersistDirectoryURL(void) {
    NSURL *containerURL = ContainerURL();
    if (containerURL == nil)
        return nil;
    return [[containerURL URLByAppendingPathComponent:@"AOK" isDirectory:YES]
            URLByAppendingPathComponent:@"persist" isDirectory:YES];
}

static NSURL *ISHLLMTranscriptURL(void) {
    NSURL *directoryURL = ISHLLMPersistDirectoryURL();
    return [directoryURL URLByAppendingPathComponent:@"llm-chat.json" isDirectory:NO];
}

static NSURL *ISHLLMExtractsDirectoryURL(void) {
    return [ISHLLMPersistDirectoryURL() URLByAppendingPathComponent:@"llm-extracts" isDirectory:YES];
}

static NSString *ISHLLMSanitizeFilenameComponent(NSString *value) {
    NSMutableString *result = [NSMutableString string];
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-" ];
    for (NSUInteger i = 0; i < value.length; i++) {
        unichar ch = [value characterAtIndex:i];
        if ([allowed characterIsMember:ch])
            [result appendFormat:@"%C", ch];
    }
    return result.length > 0 ? result : @"snippet";
}

static NSString *ISHLLMExtensionForFenceLanguage(NSString *language) {
    NSString *lower = language.lowercaseString;
    if ([lower isEqualToString:@"sh"] || [lower isEqualToString:@"bash"] || [lower isEqualToString:@"zsh"] || [lower isEqualToString:@"shell"])
        return @"sh";
    if ([lower isEqualToString:@"python"] || [lower isEqualToString:@"py"])
        return @"py";
    if ([lower isEqualToString:@"javascript"] || [lower isEqualToString:@"js"])
        return @"js";
    if ([lower isEqualToString:@"typescript"] || [lower isEqualToString:@"ts"])
        return @"ts";
    if ([lower isEqualToString:@"objective-c"] || [lower isEqualToString:@"objc"] || [lower isEqualToString:@"m"])
        return @"m";
    if ([lower isEqualToString:@"c"])
        return @"c";
    if ([lower isEqualToString:@"cpp"] || [lower isEqualToString:@"c++"])
        return @"cpp";
    return @"txt";
}

#pragma mark - Chat destinations

// A "destination" is one saved endpoint: provider + server URL + model + API
// key. The four UserPreferences scalars stay the single source of truth for
// everything that actually talks to a server -- selecting a destination just
// mirrors its values into them -- so no request path had to learn about
// destinations at all. The saved array only describes the set to pick from.
//
// Because the mirror is global, two live chat view controllers (a Workspace
// window and the terminal's modal) share one active destination: the last
// switch wins for both. That matches how the four scalars already behaved.
static NSString *const kISHLLMDestinationID = @"id";
static NSString *const kISHLLMDestinationName = @"name";
static NSString *const kISHLLMDestinationProvider = @"provider";
static NSString *const kISHLLMDestinationURL = @"url";
static NSString *const kISHLLMDestinationModel = @"model";
static NSString *const kISHLLMDestinationAPIKey = @"apiKey";

static NSString *ISHLLMStringValue(NSDictionary *dictionary, NSString *key) {
    id value = dictionary[key];
    return [value isKindOfClass:NSString.class] ? value : @"";
}

// Snapshot of the live configuration as a destination entry.
static NSDictionary<NSString *, NSString *> *ISHLLMDestinationFromCurrentPreferences(NSString *identifier, NSString *name) {
    UserPreferences *preferences = UserPreferences.shared;
    NSString *provider = preferences.llmProvider ?: @"Custom";
    return @{
        kISHLLMDestinationID: identifier.length > 0 ? identifier : NSUUID.UUID.UUIDString,
        kISHLLMDestinationName: name.length > 0 ? name : provider,
        kISHLLMDestinationProvider: provider,
        kISHLLMDestinationURL: preferences.llmServerURL ?: @"",
        kISHLLMDestinationModel: preferences.llmModel ?: @"",
        kISHLLMDestinationAPIKey: preferences.llmAPIKey ?: @"",
    };
}

// Reading accessor, with the upgrade seed: someone arriving from an older
// build has exactly one configured endpoint (the four scalars), so it becomes
// destination #1 and stays selected. Entries without a usable id are dropped
// rather than trusted, since the id is what selection and editing key on.
static NSArray<NSDictionary<NSString *, NSString *> *> *ISHLLMDestinations(void) {
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *valid = [NSMutableArray array];
    for (id entry in UserPreferences.shared.llmDestinations) {
        if ([entry isKindOfClass:NSDictionary.class] && ISHLLMStringValue(entry, kISHLLMDestinationID).length > 0)
            [valid addObject:entry];
    }
    if (valid.count == 0) {
        NSDictionary<NSString *, NSString *> *seeded = ISHLLMDestinationFromCurrentPreferences(nil, nil);
        [valid addObject:seeded];
        UserPreferences.shared.llmDestinations = valid;
        UserPreferences.shared.llmActiveDestinationID = seeded[kISHLLMDestinationID];
    }
    return valid;
}

// Index of the selected destination. A stale or deleted id resolves to the
// first entry instead of failing -- there is always exactly one active
// destination, and ISHLLMDestinations() guarantees the array is non-empty.
static NSUInteger ISHLLMActiveDestinationIndex(void) {
    NSArray<NSDictionary<NSString *, NSString *> *> *destinations = ISHLLMDestinations();
    NSString *activeID = UserPreferences.shared.llmActiveDestinationID;
    if (activeID.length > 0) {
        for (NSUInteger i = 0; i < destinations.count; i++) {
            if ([ISHLLMStringValue(destinations[i], kISHLLMDestinationID) isEqualToString:activeID])
                return i;
        }
    }
    return 0;
}

static NSDictionary<NSString *, NSString *> *ISHLLMActiveDestination(void) {
    return ISHLLMDestinations()[ISHLLMActiveDestinationIndex()];
}

static NSString *ISHLLMDestinationDisplayName(NSDictionary<NSString *, NSString *> *destination) {
    NSString *name = ISHLLMStringValue(destination, kISHLLMDestinationName);
    if (name.length > 0)
        return name;
    NSString *provider = ISHLLMStringValue(destination, kISHLLMDestinationProvider);
    return provider.length > 0 ? provider : @"Destination";
}

// One-line "what does this destination point at" summary for pickers.
static NSString *ISHLLMDestinationSubtitle(NSDictionary<NSString *, NSString *> *destination) {
    NSString *model = ISHLLMStringValue(destination, kISHLLMDestinationModel);
    NSString *provider = ISHLLMStringValue(destination, kISHLLMDestinationProvider);
    if ([provider.lowercaseString containsString:@"foundation models"])
        return model.length > 0 ? [@"On-device · " stringByAppendingString:model] : @"On-device";
    NSString *host = [NSURL URLWithString:ISHLLMStringValue(destination, kISHLLMDestinationURL)].host ?: @"";
    if (model.length > 0 && host.length > 0)
        return [NSString stringWithFormat:@"%@ · %@", model, host];
    if (model.length > 0)
        return model;
    return host.length > 0 ? host : @"Not configured";
}

// The ONLY place a destination is written into the four scalars.
static void ISHLLMActivateDestination(NSDictionary<NSString *, NSString *> *destination) {
    UserPreferences *preferences = UserPreferences.shared;
    preferences.llmActiveDestinationID = ISHLLMStringValue(destination, kISHLLMDestinationID);
    preferences.llmProvider = ISHLLMStringValue(destination, kISHLLMDestinationProvider) ?: @"Custom";
    preferences.llmServerURL = ISHLLMStringValue(destination, kISHLLMDestinationURL);
    preferences.llmModel = ISHLLMStringValue(destination, kISHLLMDestinationModel);
    preferences.llmAPIKey = ISHLLMStringValue(destination, kISHLLMDestinationAPIKey);
}

// The inverse, and the reason the saved set can't drift: every edit that
// writes one of the four scalars (the Settings rows, the provider preset
// picker, the model picker, the /model command) calls this straight after, so
// the active entry always describes the live configuration.
static void ISHLLMSyncActiveDestinationFromPreferences(void) {
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *destinations = [ISHLLMDestinations() mutableCopy];
    NSUInteger index = ISHLLMActiveDestinationIndex();
    NSDictionary<NSString *, NSString *> *existing = destinations[index];
    // A destination the user never renamed follows its provider; one they did
    // name keeps that name across a provider change.
    NSString *name = ISHLLMStringValue(existing, kISHLLMDestinationName);
    if (name.length == 0 || [name isEqualToString:ISHLLMStringValue(existing, kISHLLMDestinationProvider)])
        name = nil;
    destinations[index] = ISHLLMDestinationFromCurrentPreferences(ISHLLMStringValue(existing, kISHLLMDestinationID), name);
    UserPreferences.shared.llmDestinations = destinations;
    UserPreferences.shared.llmActiveDestinationID = destinations[index][kISHLLMDestinationID];
}

// Upsert by id; a destination not already in the array is appended.
static void ISHLLMSaveDestination(NSDictionary<NSString *, NSString *> *destination) {
    NSString *identifier = ISHLLMStringValue(destination, kISHLLMDestinationID);
    if (identifier.length == 0)
        return;
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *destinations = [ISHLLMDestinations() mutableCopy];
    for (NSUInteger i = 0; i < destinations.count; i++) {
        if ([ISHLLMStringValue(destinations[i], kISHLLMDestinationID) isEqualToString:identifier]) {
            destinations[i] = destination;
            UserPreferences.shared.llmDestinations = destinations;
            // Editing the active destination has to move the live scalars too,
            // or the chat keeps talking to the pre-edit endpoint.
            if (i == ISHLLMActiveDestinationIndex())
                ISHLLMActivateDestination(destination);
            return;
        }
    }
    [destinations addObject:destination];
    UserPreferences.shared.llmDestinations = destinations;
}

// Deleting the active destination selects a survivor; the last one can't be
// deleted, since "no destination" has no meaning for the four scalars.
static BOOL ISHLLMDeleteDestinationWithID(NSString *identifier) {
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *destinations = [ISHLLMDestinations() mutableCopy];
    if (destinations.count <= 1)
        return NO;
    BOOL wasActive = [ISHLLMStringValue(ISHLLMActiveDestination(), kISHLLMDestinationID) isEqualToString:identifier];
    NSUInteger removedIndex = NSNotFound;
    for (NSUInteger i = 0; i < destinations.count; i++) {
        if ([ISHLLMStringValue(destinations[i], kISHLLMDestinationID) isEqualToString:identifier]) {
            removedIndex = i;
            break;
        }
    }
    if (removedIndex == NSNotFound)
        return NO;
    [destinations removeObjectAtIndex:removedIndex];
    UserPreferences.shared.llmDestinations = destinations;
    if (wasActive)
        ISHLLMActivateDestination(destinations[removedIndex < destinations.count ? removedIndex : destinations.count - 1]);
    return YES;
}

#pragma mark - Chat sessions

// Chats are stored one file per session under /AOK/persist/llm-chats, with an
// index document beside them naming the sessions and which one is selected:
//
//   llm-chats/index.json          {version, active, sessions:[{id,title,...}]}
//   llm-chats/<session-id>.json   the message array, same shape as before
//
// Session files use the same writeToURL:/arrayWithContentsOfURL: mechanism
// the single transcript always used (a property list, despite the .json
// name) so the on-disk message format is unchanged and a migrated chat reads
// back exactly as it was written.
static NSURL *ISHLLMSessionsDirectoryURL(void) {
    return [ISHLLMPersistDirectoryURL() URLByAppendingPathComponent:@"llm-chats" isDirectory:YES];
}

static NSURL *ISHLLMSessionIndexURL(void) {
    return [ISHLLMSessionsDirectoryURL() URLByAppendingPathComponent:@"index.json" isDirectory:NO];
}

static NSURL *ISHLLMSessionFileURL(NSString *sessionID) {
    NSString *component = ISHLLMSanitizeFilenameComponent(sessionID ?: @"");
    return [ISHLLMSessionsDirectoryURL() URLByAppendingPathComponent:[component stringByAppendingString:@".json"] isDirectory:NO];
}

// ContainerURL() is nil when the app group isn't configured, and every URL
// derived from it is then nil too. Rather than handing nil URLs to Foundation
// at four call sites, the chats simply live in memory for that launch.
static BOOL ISHLLMSessionStorageAvailable(void) {
    return ISHLLMSessionsDirectoryURL() != nil;
}

static void ISHLLMEnsureSessionsDirectory(void) {
    NSURL *directoryURL = ISHLLMSessionsDirectoryURL();
    if (directoryURL != nil)
        [NSFileManager.defaultManager createDirectoryAtURL:directoryURL withIntermediateDirectories:YES attributes:nil error:nil];
}

static NSArray<NSDictionary<NSString *, id> *> *ISHLLMValidMessagesFromStoredArray(NSArray *stored) {
    NSMutableArray<NSDictionary<NSString *, id> *> *messages = [NSMutableArray array];
    for (id message in stored) {
        if ([message isKindOfClass:NSDictionary.class] && [message[@"role"] isKindOfClass:NSString.class] && [message[@"content"] isKindOfClass:NSString.class])
            [messages addObject:message];
    }
    return messages;
}

static NSArray<NSDictionary<NSString *, id> *> *ISHLLMLoadSessionMessages(NSString *sessionID) {
    if (sessionID.length == 0 || !ISHLLMSessionStorageAvailable())
        return @[];
    NSArray *stored = [NSArray arrayWithContentsOfURL:ISHLLMSessionFileURL(sessionID)];
    return [stored isKindOfClass:NSArray.class] ? ISHLLMValidMessagesFromStoredArray(stored) : @[];
}

static void ISHLLMWriteSessionMessages(NSString *sessionID, NSArray<NSDictionary<NSString *, id> *> *messages) {
    if (sessionID.length == 0 || !ISHLLMSessionStorageAvailable())
        return;
    ISHLLMEnsureSessionsDirectory();
    [messages writeToURL:ISHLLMSessionFileURL(sessionID) atomically:YES];
}

// A one-line label for a chat, taken from its first real user turn -- the
// same thing the user would have typed as a title. Slash commands and the
// terminal-context prompts are skipped; those name the tool, not the chat.
static NSString *ISHLLMSessionTitleFromMessages(NSArray<NSDictionary<NSString *, id> *> *messages) {
    for (NSDictionary<NSString *, id> *message in messages) {
        if (![ISHLLMStringValue(message, @"role") isEqualToString:@"user"])
            continue;
        NSString *content = [ISHLLMStringValue(message, @"content") stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (content.length == 0 || [content hasPrefix:@"/"])
            continue;
        NSString *firstLine = [content componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet].firstObject ?: content;
        firstLine = [firstLine stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (firstLine.length == 0)
            continue;
        if (firstLine.length > 42)
            firstLine = [[firstLine substringToIndex:41] stringByAppendingString:@"…"];
        return firstLine;
    }
    return @"";
}

static NSDictionary<NSString *, id> *ISHLLMNewSessionEntry(NSString *title) {
    NSTimeInterval now = NSDate.date.timeIntervalSince1970;
    return @{
        @"id": NSUUID.UUID.UUIDString,
        @"title": title.length > 0 ? title : @"New Chat",
        @"created": @(now),
        @"updated": @(now),
        @"destination": ISHLLMStringValue(ISHLLMActiveDestination(), kISHLLMDestinationID),
        @"system": @"",
        @"count": @0,
    };
}

// The index, rebuilt from whatever is on disk and always valid: at least one
// session, a selected id that exists, and (on first run in this build) the
// pre-sessions transcript carried in as the first chat. The legacy
// llm-chat.json is copied, never moved -- an older build reopened afterwards
// still finds the chat where it left it.
static void ISHLLMWriteSessionIndex(NSArray<NSDictionary<NSString *, id> *> *sessions, NSString *activeID);

static NSDictionary<NSString *, id> *ISHLLMLoadSessionIndexDocument(void) {
    NSDictionary *stored = ISHLLMSessionStorageAvailable() ? [NSDictionary dictionaryWithContentsOfURL:ISHLLMSessionIndexURL()] : nil;
    NSMutableArray<NSDictionary<NSString *, id> *> *sessions = [NSMutableArray array];
    if ([stored isKindOfClass:NSDictionary.class] && [stored[@"sessions"] isKindOfClass:NSArray.class]) {
        for (id entry in stored[@"sessions"]) {
            if ([entry isKindOfClass:NSDictionary.class] && ISHLLMStringValue(entry, @"id").length > 0)
                [sessions addObject:entry];
        }
    }
    NSString *activeID = [stored isKindOfClass:NSDictionary.class] ? ISHLLMStringValue(stored, @"active") : @"";
    BOOL synthesized = NO;

    if (sessions.count == 0) {
        NSArray *legacy = ISHLLMSessionStorageAvailable() ? [NSArray arrayWithContentsOfURL:ISHLLMTranscriptURL()] : nil;
        NSArray<NSDictionary<NSString *, id> *> *legacyMessages = [legacy isKindOfClass:NSArray.class] ? ISHLLMValidMessagesFromStoredArray(legacy) : @[];
        NSMutableDictionary<NSString *, id> *entry = [ISHLLMNewSessionEntry(ISHLLMSessionTitleFromMessages(legacyMessages)) mutableCopy];
        if (legacyMessages.count > 0) {
            entry[@"count"] = @(legacyMessages.count);
            ISHLLMWriteSessionMessages(entry[@"id"], legacyMessages);
        }
        [sessions addObject:entry];
        activeID = entry[@"id"];
        synthesized = YES;
    }

    BOOL activeExists = NO;
    for (NSDictionary<NSString *, id> *entry in sessions) {
        if ([ISHLLMStringValue(entry, @"id") isEqualToString:activeID]) {
            activeExists = YES;
            break;
        }
    }
    if (!activeExists) {
        activeID = ISHLLMStringValue(sessions.firstObject, @"id");
        synthesized = YES;
    }
    // Anything invented here MUST be written back before returning. This
    // function is a read accessor called several times per switch, and each
    // call re-derives what it did not find -- so a migration that stayed in
    // memory would mint a new chat id (and a new copy of the migrated
    // transcript) on every single call, leaving the user on an empty chat with
    // their old conversation stranded in orphan files.
    if (synthesized)
        ISHLLMWriteSessionIndex(sessions, activeID);
    return @{@"version": @1, @"active": activeID, @"sessions": sessions};
}

static void ISHLLMWriteSessionIndex(NSArray<NSDictionary<NSString *, id> *> *sessions, NSString *activeID) {
    if (!ISHLLMSessionStorageAvailable())
        return;
    ISHLLMEnsureSessionsDirectory();
    [@{@"version": @1, @"active": activeID ?: @"", @"sessions": sessions ?: @[]} writeToURL:ISHLLMSessionIndexURL() atomically:YES];
}

static NSArray<NSDictionary<NSString *, id> *> *ISHLLMSessionEntries(void) {
    return ISHLLMLoadSessionIndexDocument()[@"sessions"];
}

// Newest activity first, which is the order a chat list is useful in.
static NSArray<NSDictionary<NSString *, id> *> *ISHLLMSessionEntriesByRecency(void) {
    return [ISHLLMSessionEntries() sortedArrayUsingComparator:^NSComparisonResult(NSDictionary<NSString *, id> *a, NSDictionary<NSString *, id> *b) {
        double left = [a[@"updated"] isKindOfClass:NSNumber.class] ? [a[@"updated"] doubleValue] : 0.0;
        double right = [b[@"updated"] isKindOfClass:NSNumber.class] ? [b[@"updated"] doubleValue] : 0.0;
        if (left == right)
            return NSOrderedSame;
        return left > right ? NSOrderedAscending : NSOrderedDescending;
    }];
}

static NSString *ISHLLMActiveSessionID(void) {
    return ISHLLMLoadSessionIndexDocument()[@"active"];
}

static NSDictionary<NSString *, id> *ISHLLMSessionEntryWithID(NSString *sessionID) {
    for (NSDictionary<NSString *, id> *entry in ISHLLMSessionEntries()) {
        if ([ISHLLMStringValue(entry, @"id") isEqualToString:sessionID])
            return entry;
    }
    return nil;
}

// Merge `updates` into one session entry in place, leaving the rest alone.
static void ISHLLMUpdateSessionEntry(NSString *sessionID, NSDictionary<NSString *, id> *updates) {
    if (sessionID.length == 0)
        return;
    NSDictionary<NSString *, id> *document = ISHLLMLoadSessionIndexDocument();
    NSMutableArray<NSDictionary<NSString *, id> *> *sessions = [document[@"sessions"] mutableCopy];
    for (NSUInteger i = 0; i < sessions.count; i++) {
        if (![ISHLLMStringValue(sessions[i], @"id") isEqualToString:sessionID])
            continue;
        NSMutableDictionary<NSString *, id> *entry = [sessions[i] mutableCopy];
        [entry addEntriesFromDictionary:updates];
        sessions[i] = entry;
        ISHLLMWriteSessionIndex(sessions, document[@"active"]);
        return;
    }
}

static void ISHLLMSetActiveSessionID(NSString *sessionID) {
    NSDictionary<NSString *, id> *document = ISHLLMLoadSessionIndexDocument();
    ISHLLMWriteSessionIndex(document[@"sessions"], sessionID);
}

static NSDictionary<NSString *, id> *ISHLLMCreateSession(NSString *title) {
    NSDictionary<NSString *, id> *document = ISHLLMLoadSessionIndexDocument();
    NSMutableArray<NSDictionary<NSString *, id> *> *sessions = [document[@"sessions"] mutableCopy];
    NSDictionary<NSString *, id> *entry = ISHLLMNewSessionEntry(title);
    [sessions addObject:entry];
    ISHLLMWriteSessionIndex(sessions, entry[@"id"]);
    return entry;
}

// Deleting the last chat leaves an empty one rather than no chat at all --
// the client always has somewhere to put the next message.
static NSString *ISHLLMDeleteSession(NSString *sessionID) {
    NSDictionary<NSString *, id> *document = ISHLLMLoadSessionIndexDocument();
    NSMutableArray<NSDictionary<NSString *, id> *> *sessions = [document[@"sessions"] mutableCopy];
    NSUInteger removedIndex = NSNotFound;
    for (NSUInteger i = 0; i < sessions.count; i++) {
        if ([ISHLLMStringValue(sessions[i], @"id") isEqualToString:sessionID]) {
            removedIndex = i;
            break;
        }
    }
    if (removedIndex == NSNotFound)
        return ISHLLMStringValue(document, @"active");
    [sessions removeObjectAtIndex:removedIndex];
    if (ISHLLMSessionStorageAvailable())
        [NSFileManager.defaultManager removeItemAtURL:ISHLLMSessionFileURL(sessionID) error:nil];
    if (sessions.count == 0)
        [sessions addObject:ISHLLMNewSessionEntry(nil)];
    NSString *activeID = ISHLLMStringValue(document, @"active");
    if ([activeID isEqualToString:sessionID])
        activeID = ISHLLMStringValue(sessions[removedIndex < sessions.count ? removedIndex : sessions.count - 1], @"id");
    ISHLLMWriteSessionIndex(sessions, activeID);
    return activeID;
}

static NSString *ISHLLMChatEndpoint(void) {
    NSString *base = [UserPreferences.shared.llmServerURL stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (base.length == 0)
        base = @"http://localhost:11434/v1";
    while ([base hasSuffix:@"/"])
        base = [base substringToIndex:base.length - 1];
    if ([base hasSuffix:@"/chat/completions"])
        return base;
    return [base stringByAppendingString:@"/chat/completions"];
}

static BOOL ISHLLMUsesAppleFoundationModels(void) {
    return [UserPreferences.shared.llmProvider.lowercaseString containsString:@"foundation models"];
}

static AOKLLMBackend ISHLLMCurrentBackend(void) {
    return ISHLLMUsesAppleFoundationModels()
        ? AOKLLMBackendAppleFoundationModels
        : AOKLLMBackendOpenAICompatibleEndpoint;
}

// Status/explanation text for the Apple Foundation Models provider. When the
// Swift bridge is compiled in (libiSH_AOKApp-Swift.h present) this reflects
// the live SystemLanguageModel.default.availability on this device/OS; older
// builds without FoundationModels.framework linked fall back to a static
// explanation.
static NSString *ISHLLMAppleFoundationModelsUnavailableMessage(void) {
#if __has_include("libiSH_AOKApp-Swift.h")
    return [AOKFoundationModelsBridge availabilityDescription];
#else
    return @"Apple Foundation Models is selected, but this build cannot call it yet. The current SDK does not expose FoundationModels.framework, and the app still targets older iOS/iPadOS. When built with the iOS 26 SDK, this provider should use Apple's on-device Foundation Models backend with no server URL or API key.";
#endif
}

static BOOL ISHLLMFoundationModelsReady(void) {
#if __has_include("libiSH_AOKApp-Swift.h")
    return [AOKFoundationModelsBridge currentAvailability] == AOKFoundationModelAvailabilityAvailable;
#else
    return NO;
#endif
}

static BOOL ISHLLMUsesGeminiAPI(void) {
    if (ISHLLMUsesAppleFoundationModels())
        return NO;
    NSString *provider = UserPreferences.shared.llmProvider.lowercaseString;
    NSString *host = [NSURL URLWithString:UserPreferences.shared.llmServerURL].host.lowercaseString ?: @"";
    return [provider containsString:@"gemini"] || [host containsString:@"generativelanguage.googleapis.com"];
}

static NSString *ISHLLMGeminiGenerateEndpoint(void) {
    NSString *base = [UserPreferences.shared.llmServerURL stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (base.length == 0)
        base = @"https://generativelanguage.googleapis.com/v1beta";
    while ([base hasSuffix:@"/"])
        base = [base substringToIndex:base.length - 1];
    NSString *model = [UserPreferences.shared.llmModel stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (![model hasPrefix:@"models/"])
        model = [@"models/" stringByAppendingString:model];
    NSString *encodedModel = [model stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet] ?: model;
    NSString *endpoint = [base stringByAppendingFormat:@"/%@:generateContent", encodedModel];
    NSString *apiKey = [UserPreferences.shared.llmAPIKey stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLQueryAllowedCharacterSet] ?: @"";
    return apiKey.length > 0 ? [endpoint stringByAppendingFormat:@"?key=%@", apiKey] : endpoint;
}

static NSString *ISHLLMModelsEndpoint(void) {
    if (ISHLLMUsesGeminiAPI()) {
        NSString *base = [UserPreferences.shared.llmServerURL stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (base.length == 0)
            base = @"https://generativelanguage.googleapis.com/v1beta";
        while ([base hasSuffix:@"/"])
            base = [base substringToIndex:base.length - 1];
        NSString *apiKey = [UserPreferences.shared.llmAPIKey stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLQueryAllowedCharacterSet] ?: @"";
        NSString *endpoint = [base stringByAppendingString:@"/models"];
        return apiKey.length > 0 ? [endpoint stringByAppendingFormat:@"?key=%@", apiKey] : endpoint;
    }
    NSString *base = [UserPreferences.shared.llmServerURL stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (base.length == 0)
        base = @"https://openrouter.ai/api/v1";
    while ([base hasSuffix:@"/"])
        base = [base substringToIndex:base.length - 1];
    if ([base hasSuffix:@"/chat/completions"])
        base = [base substringToIndex:base.length - @"/chat/completions".length];
    if ([base hasSuffix:@"/models"])
        return base;
    return [base stringByAppendingString:@"/models"];
}

static void ISHConfigureLLMSettingsNavigationController(UINavigationController *navigationController) {
    if (@available(iOS 13.0, *)) {
        navigationController.modalPresentationStyle = UIModalPresentationFormSheet;
    } else {
        navigationController.modalPresentationStyle = UIModalPresentationPageSheet;
    }
}

static NSArray<NSDictionary<NSString *, NSString *> *> *ISHLLMProviderPresets(void) {
    return @[
        @{@"name": @"Apple Foundation Models", @"url": @"", @"model": @"system-language-model", @"format": @"Apple on-device Foundation Models"},
        @{@"name": @"OpenRouter Free", @"url": @"https://openrouter.ai/api/v1", @"model": @"openrouter/free", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Groq Llama", @"url": @"https://api.groq.com/openai/v1", @"model": @"llama-3.1-8b-instant", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Gemini Flash", @"url": @"https://generativelanguage.googleapis.com/v1beta", @"model": @"gemini-2.5-flash", @"format": @"Google Gemini generateContent"},
        @{@"name": @"LM Studio", @"url": @"http://127.0.0.1:1234/v1", @"model": @"local-model", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Ollama", @"url": @"http://127.0.0.1:11434/v1", @"model": @"llama3.2", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"OpenAI", @"url": @"https://api.openai.com/v1", @"model": @"gpt-4o-mini", @"format": @"OpenAI-compatible chat completions"},
        @{@"name": @"Custom", @"url": @"", @"model": @"", @"format": @"OpenAI-compatible chat completions"},
    ];
}

static NSString *ISHLLMCurrentAPIFormat(void) {
    if (ISHLLMUsesAppleFoundationModels())
        return @"Apple on-device Foundation Models";
    if (ISHLLMUsesGeminiAPI())
        return @"Google Gemini generateContent";
    return @"OpenAI-compatible chat completions";
}

static BOOL ISHLLMProviderRequiresAPIKey(void) {
    if (ISHLLMUsesAppleFoundationModels())
        return NO;
    NSString *provider = UserPreferences.shared.llmProvider.lowercaseString;
    NSString *host = [NSURL URLWithString:UserPreferences.shared.llmServerURL].host.lowercaseString ?: @"";
    return [provider containsString:@"openrouter"] || [provider containsString:@"openai"] ||
        [provider containsString:@"groq"] || [provider containsString:@"gemini"] ||
        [host containsString:@"openrouter.ai"] || [host containsString:@"api.openai.com"] ||
        [host containsString:@"api.groq.com"] || [host containsString:@"generativelanguage.googleapis.com"];
}

static NSString *ISHLLMMissingAPIKeyMessage(void) {
    return @"This provider requires an API key. Add it in LLM Settings -> API Key. OpenRouter free, Groq, Gemini, and OpenAI all require API keys.";
}

static NSArray<NSString *> *ISHLLMModelIdentifiersFromResponseData(NSData *data) {
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![json isKindOfClass:NSDictionary.class])
        return @[];
    NSArray *models = ISHLLMUsesGeminiAPI() ? json[@"models"] : json[@"data"];
    if (![models isKindOfClass:NSArray.class])
        return @[];
    NSMutableArray<NSString *> *ids = [NSMutableArray array];
    for (id model in models) {
        if (![model isKindOfClass:NSDictionary.class])
            continue;
        if (ISHLLMUsesGeminiAPI()) {
            NSArray *methods = model[@"supportedGenerationMethods"];
            if ([methods isKindOfClass:NSArray.class] && ![methods containsObject:@"generateContent"])
                continue;
            NSString *name = [model[@"name"] isKindOfClass:NSString.class] ? model[@"name"] : nil;
            if ([name hasPrefix:@"models/"])
                name = [name substringFromIndex:@"models/".length];
            if (name.length > 0)
                [ids addObject:name];
        } else if ([model[@"id"] isKindOfClass:NSString.class]) {
            [ids addObject:model[@"id"]];
        }
    }
    return [ids sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
}

// Best-effort context-window (max input tokens) lookup for one model entry in
// a /models (or Gemini ListModels) response. There's no standard field for
// this -- OpenRouter uses "context_length" (sometimes only nested under
// "top_provider"), Groq uses "context_window", vLLM's OpenAI-compatible
// server adds "max_model_len", Gemini's ListModels uses "inputTokenLimit".
// Plain OpenAI/Ollama don't expose it at all. Returns 0 if the model or a
// usable field can't be found -- callers must treat that as "unknown", not 0.
static NSInteger ISHLLMContextWindowFromModelsResponse(NSData *data, NSString *modelID) {
    if (modelID.length == 0)
        return 0;
    id json = data.length > 0 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![json isKindOfClass:NSDictionary.class])
        return 0;
    NSArray *models = ISHLLMUsesGeminiAPI() ? json[@"models"] : json[@"data"];
    if (![models isKindOfClass:NSArray.class])
        return 0;
    for (id entry in models) {
        if (![entry isKindOfClass:NSDictionary.class])
            continue;
        NSDictionary *model = entry;
        NSString *entryID;
        if (ISHLLMUsesGeminiAPI()) {
            entryID = [model[@"name"] isKindOfClass:NSString.class] ? model[@"name"] : nil;
            if ([entryID hasPrefix:@"models/"])
                entryID = [entryID substringFromIndex:@"models/".length];
        } else {
            entryID = [model[@"id"] isKindOfClass:NSString.class] ? model[@"id"] : nil;
        }
        if (entryID.length == 0 || ![entryID isEqualToString:modelID])
            continue;
        for (NSString *key in @[@"context_length", @"context_window", @"max_model_len", @"max_context_length", @"n_ctx"]) {
            id value = model[key];
            if ([value isKindOfClass:NSNumber.class] && [value integerValue] > 0)
                return [value integerValue];
        }
        NSDictionary *topProvider = [model[@"top_provider"] isKindOfClass:NSDictionary.class] ? model[@"top_provider"] : nil;
        id nestedLength = topProvider[@"context_length"];
        if ([nestedLength isKindOfClass:NSNumber.class] && [nestedLength integerValue] > 0)
            return [nestedLength integerValue];
        if (ISHLLMUsesGeminiAPI()) {
            id inputLimit = model[@"inputTokenLimit"];
            if ([inputLimit isKindOfClass:NSNumber.class] && [inputLimit integerValue] > 0)
                return [inputLimit integerValue];
        }
        return 0; // matched the model but no known context-size field
    }
    return 0;
}

static NSString *ISHLLMSanitizedAssistantContent(NSString *content) {
    NSRange fileSeparator = [content rangeOfString:@"<file_sep>"];
    if (fileSeparator.location != NSNotFound)
        content = [content substringToIndex:fileSeparator.location];
    return [content stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

// Streaming-safe accumulator. Unlike ISHLLMSanitizedAssistantContent, this must
// NOT trim trailing whitespace: it runs on the whole running message after every
// streamed delta, so a token-boundary space that lands at the tail of the buffer
// between two deltas (e.g. delta "Hello! " followed by "How") would be stripped,
// gluing the next word on ("Hello!How", "helpyou"). We only cut at the <file_sep>
// stop marker and trim *leading* whitespace, which is idempotent once real text
// has arrived and never touches interior or trailing spaces. Call
// ISHLLMSanitizedAssistantContent once at end-of-stream for the final cleanup.
static NSString *ISHLLMStreamingAssistantContent(NSString *content) {
    NSRange fileSeparator = [content rangeOfString:@"<file_sep>"];
    if (fileSeparator.location != NSNotFound)
        content = [content substringToIndex:fileSeparator.location];
    NSCharacterSet *whitespace = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    NSUInteger start = 0;
    while (start < content.length && [whitespace characterIsMember:[content characterAtIndex:start]])
        start++;
    return start > 0 ? [content substringFromIndex:start] : content;
}

static BOOL ISHLLMHideThinkingEnabled(void) {
    return UserPreferences.shared.llmHideThinking;
}

// Ranges of `content` that are literal code -- fenced blocks and inline
// backtick spans. Tags inside them are being *talked about* (ask a model how
// this very feature works and it will happily print a <think> tag in an
// example), so they must not be mistaken for the model thinking. An unclosed
// fence runs to the end of the text, which is also right mid-stream.
static NSArray<NSValue *> *ISHLLMCodeSpanRanges(NSString *content) {
    static NSRegularExpression *fenceMarker;
    static NSRegularExpression *inlineSpan;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        fenceMarker = [NSRegularExpression regularExpressionWithPattern:@"^[ \\t]*```" options:NSRegularExpressionAnchorsMatchLines error:NULL];
        inlineSpan = [NSRegularExpression regularExpressionWithPattern:@"`[^`\\n]*`" options:0 error:NULL];
    });
    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    if (fenceMarker == nil || inlineSpan == nil)
        return ranges;
    NSRange whole = NSMakeRange(0, content.length);
    NSArray<NSTextCheckingResult *> *fences = [fenceMarker matchesInString:content options:0 range:whole];
    for (NSUInteger i = 0; i < fences.count; i += 2) {
        NSUInteger start = fences[i].range.location;
        NSUInteger end = i + 1 < fences.count ? NSMaxRange(fences[i + 1].range) : content.length;
        [ranges addObject:[NSValue valueWithRange:NSMakeRange(start, end - start)]];
    }
    NSUInteger fencedCount = ranges.count;
    for (NSTextCheckingResult *match in [inlineSpan matchesInString:content options:0 range:whole]) {
        BOOL insideFence = NO;
        for (NSUInteger i = 0; i < fencedCount && !insideFence; i++)
            insideFence = NSLocationInRange(match.range.location, ranges[i].rangeValue);
        if (!insideFence)
            [ranges addObject:[NSValue valueWithRange:match.range]];
    }
    return ranges;
}

static NSTextCheckingResult *ISHLLMFirstTagOutsideCode(NSRegularExpression *regex,
                                                       NSString *content,
                                                       NSRange range,
                                                       NSArray<NSValue *> *codeRanges) {
    __block NSTextCheckingResult *found = nil;
    [regex enumerateMatchesInString:content options:0 range:range usingBlock:^(NSTextCheckingResult *match, __unused NSMatchingFlags flags, BOOL *stop) {
        if (match == nil)
            return;
        for (NSValue *value in codeRanges) {
            NSRange code = value.rangeValue;
            if (match.range.location >= code.location && NSMaxRange(match.range) <= NSMaxRange(code))
                return;
        }
        found = match;
        *stop = YES;
    }];
    return found;
}

// Splits an assistant message into the part meant for the reader and the
// model's chain-of-thought. Reasoning models (DeepSeek-R1, Qwen3, gpt-oss,
// magistral, ...) emit the latter inline, wrapped in <think>...</think>; some
// spell the tag <thinking>, <thought> or <reasoning>. Returns the visible text
// with the blocks removed; each thought is appended to `thoughts` in order, and
// `*openOut` is YES when the text ends inside an unterminated block -- i.e. the
// model is thinking right now, mid-stream.
//
// Two shapes matter beyond the obvious one:
//   - Only a closing tag. Several servers apply a chat template that already
//     opens <think> for the model, so the reply starts mid-thought and the
//     first tag seen is </think>. Treated as an implicit open, but only while
//     nothing visible has been emitted yet, so a stray </think> further down a
//     normal answer stays literal text rather than eating the whole reply.
//   - No trailing trim. This runs on the accumulating buffer after every
//     streamed delta, so trimming the tail would eat token-boundary spaces
//     (see ISHLLMStreamingAssistantContent); only the leading edge is trimmed.
static NSString *ISHLLMSplitThinkingFromContent(NSString *content,
                                                NSMutableArray<NSString *> *thoughts,
                                                BOOL *openOut) {
    if (openOut != NULL)
        *openOut = NO;
    if (content.length == 0)
        return content ?: @"";
    static NSRegularExpression *openTag;
    static NSRegularExpression *closeTag;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        NSRegularExpressionOptions options = NSRegularExpressionCaseInsensitive;
        openTag = [NSRegularExpression regularExpressionWithPattern:@"<(think|thinking|thought|reasoning)>" options:options error:NULL];
        closeTag = [NSRegularExpression regularExpressionWithPattern:@"</(think|thinking|thought|reasoning)>" options:options error:NULL];
    });
    if (openTag == nil || closeTag == nil)
        return content;

    NSArray<NSValue *> *codeRanges = ISHLLMCodeSpanRanges(content);
    NSMutableString *visible = [NSMutableString string];
    NSUInteger cursor = 0;
    while (cursor < content.length) {
        NSRange remaining = NSMakeRange(cursor, content.length - cursor);
        NSTextCheckingResult *open = ISHLLMFirstTagOutsideCode(openTag, content, remaining, codeRanges);
        NSTextCheckingResult *close = ISHLLMFirstTagOutsideCode(closeTag, content, remaining, codeRanges);
        if (open == nil && close == nil) {
            [visible appendString:[content substringFromIndex:cursor]];
            break;
        }
        // Implicit open (see above): a close tag ahead of any open tag, with
        // nothing shown yet, means the whole prefix was the model thinking.
        if (open == nil || (close != nil && close.range.location < open.range.location)) {
            if (visible.length > 0) {
                // Mid-answer stray close tag: leave it alone as literal text.
                [visible appendString:[content substringWithRange:NSMakeRange(cursor, NSMaxRange(close.range) - cursor)]];
                cursor = NSMaxRange(close.range);
                continue;
            }
            NSString *thought = [content substringWithRange:NSMakeRange(cursor, close.range.location - cursor)];
            thought = [thought stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            if (thought.length > 0)
                [thoughts addObject:thought];
            cursor = NSMaxRange(close.range);
            continue;
        }
        [visible appendString:[content substringWithRange:NSMakeRange(cursor, open.range.location - cursor)]];
        NSUInteger bodyStart = NSMaxRange(open.range);
        NSRange afterOpen = NSMakeRange(bodyStart, content.length - bodyStart);
        NSTextCheckingResult *end = ISHLLMFirstTagOutsideCode(closeTag, content, afterOpen, codeRanges);
        NSString *thought = end != nil
            ? [content substringWithRange:NSMakeRange(bodyStart, end.range.location - bodyStart)]
            : [content substringFromIndex:bodyStart];
        thought = [thought stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (thought.length > 0)
            [thoughts addObject:thought];
        if (end == nil) {
            if (openOut != NULL)
                *openOut = YES;
            break;
        }
        cursor = NSMaxRange(end.range);
    }

    // Leading-only trim: idempotent once real text arrives (the blank line a
    // model leaves after </think> would otherwise open every reply).
    NSCharacterSet *whitespace = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    NSUInteger start = 0;
    while (start < visible.length && [whitespace characterIsMember:[visible characterAtIndex:start]])
        start++;
    return start > 0 ? [visible substringFromIndex:start] : visible;
}

static NSData *ISHLLMDirectHTTPPost(NSURL *url, NSData *body, NSString *apiKey, NSInteger *statusCodeOut, NSError **errorOut) {
    NSString *host = url.host;
    if (host.length == 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadURL userInfo:nil];
        return nil;
    }

    NSString *portString = url.port != nil ? url.port.stringValue : @"80";
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *results = NULL;
    int gai = getaddrinfo(host.UTF8String, portString.UTF8String, &hints, &results);
    if (gai != 0) {
        if (errorOut != nil) {
            NSString *message = [NSString stringWithUTF8String:gai_strerror(gai)] ?: @"Name lookup failed";
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:gai userInfo:@{NSLocalizedDescriptionKey: message}];
        }
        return nil;
    }

    int connectErrno = 0;
    int fd = ISHLLMConnectWithTimeout(results, 15000, &connectErrno);
    freeaddrinfo(results);
    if (fd < 0) {
        if (errorOut != nil)
            *errorOut = ISHLLMConnectionError(host, portString, connectErrno);
        return nil;
    }

    NSString *path = url.path.length > 0 ? url.path : @"/";
    if (url.query.length > 0)
        path = [path stringByAppendingFormat:@"?%@", url.query];
    NSString *hostHeader = host;
    if (url.port != nil)
        hostHeader = [hostHeader stringByAppendingFormat:@":%@", url.port];
    NSMutableString *headers = [NSMutableString stringWithFormat:
        @"POST %@ HTTP/1.1\r\n"
        @"Host: %@\r\n"
        @"Content-Type: application/json\r\n"
        @"Content-Length: %lu\r\n"
        @"Connection: close\r\n",
        path, hostHeader, (unsigned long) body.length];
    if (apiKey.length > 0)
        [headers appendFormat:@"Authorization: Bearer %@\r\n", apiKey];
    [headers appendString:@"\r\n"];

    NSMutableData *requestData = [NSMutableData dataWithData:[headers dataUsingEncoding:NSUTF8StringEncoding]];
    [requestData appendData:body];
    const uint8_t *bytes = requestData.bytes;
    size_t remaining = requestData.length;
    while (remaining > 0) {
        ssize_t sent = send(fd, bytes, remaining, 0);
        if (sent <= 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        bytes += sent;
        remaining -= (size_t) sent;
    }

    NSMutableData *responseData = [NSMutableData data];
    uint8_t buffer[8192];
    for (;;) {
        ssize_t nread = recv(fd, buffer, sizeof(buffer), 0);
        if (nread < 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        if (nread == 0)
            break;
        [responseData appendBytes:buffer length:(NSUInteger) nread];
    }
    close(fd);

    NSData *separator = [@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding];
    NSRange separatorRange = [responseData rangeOfData:separator options:0 range:NSMakeRange(0, responseData.length)];
    if (separatorRange.location == NSNotFound) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCannotParseResponse userInfo:nil];
        return nil;
    }

    NSData *headerData = [responseData subdataWithRange:NSMakeRange(0, separatorRange.location)];
    NSString *headerText = [[NSString alloc] initWithData:headerData encoding:NSISOLatin1StringEncoding] ?: @"";
    NSArray<NSString *> *headerLines = [headerText componentsSeparatedByString:@"\r\n"];
    NSArray<NSString *> *statusParts = [headerLines.firstObject componentsSeparatedByString:@" "];
    if (statusParts.count >= 2 && statusCodeOut != NULL)
        *statusCodeOut = statusParts[1].integerValue;

    NSUInteger bodyOffset = NSMaxRange(separatorRange);
    return [responseData subdataWithRange:NSMakeRange(bodyOffset, responseData.length - bodyOffset)];
}

static NSData *ISHLLMDirectHTTPGet(NSURL *url, NSString *apiKey, NSInteger *statusCodeOut, NSError **errorOut) {
    NSString *host = url.host;
    if (host.length == 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadURL userInfo:nil];
        return nil;
    }
    NSString *portString = url.port != nil ? url.port.stringValue : @"80";
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *results = NULL;
    int gai = getaddrinfo(host.UTF8String, portString.UTF8String, &hints, &results);
    if (gai != 0) {
        if (errorOut != nil) {
            NSString *message = [NSString stringWithUTF8String:gai_strerror(gai)] ?: @"Name lookup failed";
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:gai userInfo:@{NSLocalizedDescriptionKey: message}];
        }
        return nil;
    }
    int connectErrno = 0;
    int fd = ISHLLMConnectWithTimeout(results, 15000, &connectErrno);
    freeaddrinfo(results);
    if (fd < 0) {
        if (errorOut != nil)
            *errorOut = ISHLLMConnectionError(host, portString, connectErrno);
        return nil;
    }
    NSString *path = url.path.length > 0 ? url.path : @"/";
    if (url.query.length > 0)
        path = [path stringByAppendingFormat:@"?%@", url.query];
    NSString *hostHeader = host;
    if (url.port != nil)
        hostHeader = [hostHeader stringByAppendingFormat:@":%@", url.port];
    NSMutableString *requestText = [NSMutableString stringWithFormat:
        @"GET %@ HTTP/1.1\r\n"
        @"Host: %@\r\n"
        @"Accept: application/json\r\n"
        @"Connection: close\r\n",
        path, hostHeader];
    if (apiKey.length > 0)
        [requestText appendFormat:@"Authorization: Bearer %@\r\n", apiKey];
    [requestText appendString:@"\r\n"];
    NSData *requestData = [requestText dataUsingEncoding:NSUTF8StringEncoding];
    const uint8_t *bytes = requestData.bytes;
    size_t remaining = requestData.length;
    while (remaining > 0) {
        ssize_t sent = send(fd, bytes, remaining, 0);
        if (sent <= 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        bytes += sent;
        remaining -= (size_t) sent;
    }
    NSMutableData *responseData = [NSMutableData data];
    uint8_t buffer[8192];
    for (;;) {
        ssize_t nread = recv(fd, buffer, sizeof(buffer), 0);
        if (nread < 0) {
            int savedErrno = errno;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return nil;
        }
        if (nread == 0)
            break;
        [responseData appendBytes:buffer length:(NSUInteger) nread];
    }
    close(fd);
    NSData *separator = [@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding];
    NSRange separatorRange = [responseData rangeOfData:separator options:0 range:NSMakeRange(0, responseData.length)];
    if (separatorRange.location == NSNotFound)
        return responseData;
    NSData *headerData = [responseData subdataWithRange:NSMakeRange(0, separatorRange.location)];
    NSString *headerText = [[NSString alloc] initWithData:headerData encoding:NSISOLatin1StringEncoding] ?: @"";
    NSArray<NSString *> *statusParts = [[headerText componentsSeparatedByString:@"\r\n"].firstObject componentsSeparatedByString:@" "];
    if (statusParts.count >= 2 && statusCodeOut != NULL)
        *statusCodeOut = statusParts[1].integerValue;
    NSUInteger bodyOffset = NSMaxRange(separatorRange);
    return [responseData subdataWithRange:NSMakeRange(bodyOffset, responseData.length - bodyOffset)];
}

// Fetch ISHLLMModelsEndpoint() and hand the raw response to `completion` on an
// unspecified background thread (callers hop to main themselves) -- used for
// the silent context-window probe below. http endpoints use the raw-socket
// path (ATS blocks plaintext http via NSURLSession); https uses NSURLSession.
static void ISHLLMFetchModelsDataAsync(void (^completion)(NSData *data, NSInteger statusCode, NSError *error)) {
    NSURL *url = [NSURL URLWithString:ISHLLMModelsEndpoint()];
    if (url == nil) {
        completion(nil, 0, [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadURL userInfo:nil]);
        return;
    }
    NSString *apiKey = UserPreferences.shared.llmAPIKey;
    if ([url.scheme.lowercaseString isEqualToString:@"http"]) {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            NSInteger statusCode = 0;
            NSError *error = nil;
            NSData *data = ISHLLMDirectHTTPGet(url, apiKey, &statusCode, &error);
            completion(data, statusCode, error);
        });
        return;
    }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    if (apiKey.length > 0 && !ISHLLMUsesGeminiAPI())
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
    NSURLSessionDataTask *task = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
        completion(data, http.statusCode, error);
    }];
    [task resume];
}

static NSString *ISHLLMContentFromStreamingPayload(NSString *payload) {
    NSData *data = [payload dataUsingEncoding:NSUTF8StringEncoding];
    if (data.length == 0)
        return nil;
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![json isKindOfClass:NSDictionary.class])
        return nil;
    NSArray *choices = json[@"choices"];
    NSDictionary *choice = choices.count > 0 && [choices[0] isKindOfClass:NSDictionary.class] ? choices[0] : nil;
    NSDictionary *delta = [choice[@"delta"] isKindOfClass:NSDictionary.class] ? choice[@"delta"] : nil;
    NSString *content = [delta[@"content"] isKindOfClass:NSString.class] ? delta[@"content"] : nil;
    if (content.length == 0) {
        NSDictionary *message = [choice[@"message"] isKindOfClass:NSDictionary.class] ? choice[@"message"] : nil;
        content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : nil;
    }
    return content;
}

// fdOut, if non-NULL, receives the connected socket fd for as long as this
// call is blocked in send()/recv() -- so a caller on another thread can
// shutdown() it to unblock a cancelled request. Always left at 0 on return.
static BOOL ISHLLMDirectHTTPPostStreaming(NSURL *url,
                                          NSData *body,
                                          NSString *apiKey,
                                          void (^chunkHandler)(NSString *chunk),
                                          int *fdOut,
                                          NSInteger *statusCodeOut,
                                          NSError **errorOut) {
    NSString *host = url.host;
    if (host.length == 0) {
        if (errorOut != nil)
            *errorOut = [NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorBadURL userInfo:nil];
        return NO;
    }

    NSString *portString = url.port != nil ? url.port.stringValue : @"80";
    struct addrinfo hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *results = NULL;
    int gai = getaddrinfo(host.UTF8String, portString.UTF8String, &hints, &results);
    if (gai != 0) {
        if (errorOut != nil) {
            NSString *message = [NSString stringWithUTF8String:gai_strerror(gai)] ?: @"Name lookup failed";
            *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:gai userInfo:@{NSLocalizedDescriptionKey: message}];
        }
        return NO;
    }

    int connectErrno = 0;
    int fd = ISHLLMConnectWithTimeout(results, 15000, &connectErrno);
    freeaddrinfo(results);
    if (fd < 0) {
        if (errorOut != nil)
            *errorOut = ISHLLMConnectionError(host, portString, connectErrno);
        return NO;
    }
    if (fdOut != NULL)
        *fdOut = fd;

    NSString *path = url.path.length > 0 ? url.path : @"/";
    if (url.query.length > 0)
        path = [path stringByAppendingFormat:@"?%@", url.query];
    NSString *hostHeader = host;
    if (url.port != nil)
        hostHeader = [hostHeader stringByAppendingFormat:@":%@", url.port];
    NSMutableString *headers = [NSMutableString stringWithFormat:
        @"POST %@ HTTP/1.0\r\n"
        @"Host: %@\r\n"
        @"Content-Type: application/json\r\n"
        @"Accept: text/event-stream\r\n"
        @"Content-Length: %lu\r\n",
        path, hostHeader, (unsigned long) body.length];
    if (apiKey.length > 0)
        [headers appendFormat:@"Authorization: Bearer %@\r\n", apiKey];
    [headers appendString:@"\r\n"];

    NSMutableData *requestData = [NSMutableData dataWithData:[headers dataUsingEncoding:NSUTF8StringEncoding]];
    [requestData appendData:body];
    const uint8_t *bytes = requestData.bytes;
    size_t remaining = requestData.length;
    while (remaining > 0) {
        ssize_t sent = send(fd, bytes, remaining, 0);
        if (sent <= 0) {
            int savedErrno = errno;
            if (fdOut != NULL) *fdOut = 0;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return NO;
        }
        bytes += sent;
        remaining -= (size_t) sent;
    }

    NSMutableData *bufferedData = [NSMutableData data];
    NSMutableString *eventBuffer = [NSMutableString string];
    BOOL parsedHeaders = NO;
    uint8_t buffer[4096];
    for (;;) {
        ssize_t nread = recv(fd, buffer, sizeof(buffer), 0);
        if (nread < 0) {
            int savedErrno = errno;
            if (fdOut != NULL) *fdOut = 0;
            close(fd);
            if (errorOut != nil)
                *errorOut = [NSError errorWithDomain:NSPOSIXErrorDomain code:savedErrno userInfo:nil];
            return NO;
        }
        if (nread == 0)
            break;
        [bufferedData appendBytes:buffer length:(NSUInteger) nread];

        if (!parsedHeaders) {
            NSData *separator = [@"\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding];
            NSRange separatorRange = [bufferedData rangeOfData:separator options:0 range:NSMakeRange(0, bufferedData.length)];
            if (separatorRange.location == NSNotFound)
                continue;
            NSData *headerData = [bufferedData subdataWithRange:NSMakeRange(0, separatorRange.location)];
            NSString *headerText = [[NSString alloc] initWithData:headerData encoding:NSISOLatin1StringEncoding] ?: @"";
            NSArray<NSString *> *statusParts = [[headerText componentsSeparatedByString:@"\r\n"].firstObject componentsSeparatedByString:@" "];
            if (statusParts.count >= 2 && statusCodeOut != NULL)
                *statusCodeOut = statusParts[1].integerValue;
            NSUInteger bodyOffset = NSMaxRange(separatorRange);
            NSData *remainingBody = [bufferedData subdataWithRange:NSMakeRange(bodyOffset, bufferedData.length - bodyOffset)];
            [bufferedData setData:remainingBody];
            parsedHeaders = YES;
        }

        NSString *text = [[NSString alloc] initWithData:bufferedData encoding:NSUTF8StringEncoding];
        if (text.length == 0)
            continue;
        [bufferedData setLength:0];
        [eventBuffer appendString:text];
        for (;;) {
            NSRange newlineRange = [eventBuffer rangeOfString:@"\n"];
            if (newlineRange.location == NSNotFound)
                break;
            NSString *line = [[eventBuffer substringToIndex:newlineRange.location] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            [eventBuffer deleteCharactersInRange:NSMakeRange(0, NSMaxRange(newlineRange))];
            if (![line hasPrefix:@"data:"])
                continue;
            NSString *payload = [[line substringFromIndex:5] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            if ([payload isEqualToString:@"[DONE]"])
                continue;
            NSString *content = ISHLLMContentFromStreamingPayload(payload);
            if (content.length > 0 && chunkHandler != nil)
                chunkHandler(content);
        }
    }
    if (fdOut != NULL) *fdOut = 0;
    close(fd);
    return YES;
}

// MARK: - Streaming over https (NSURLSession)

// The raw-socket streamer above exists because ATS blocks plain http, so
// local Ollama/LM Studio endpoints have to be spoken to directly. https
// endpoints go through NSURLSession, which used to mean the completion-handler
// API and therefore no streaming at all: every hosted provider sat silent for
// the whole generation and then dumped the finished answer.
//
// A data-task delegate gives the same incremental Server-Sent Events the
// direct path parses, so both transports now stream. Everything before the
// first token is identical to the non-streaming path, and the fallbacks are
// the ones that already existed: a non-200, or a server that ignores
// "stream": true and answers with an ordinary JSON body, ends with zero
// chunks and the buffered body is handed to the normal response handler.
@interface ISHLLMStreamingResponseDelegate : NSObject <NSURLSessionDataDelegate>
@property (nonatomic, copy) void (^chunkHandler)(NSString *chunk);
// receivedChunks is what tells the caller whether the reply already went on
// screen incrementally or still has to be parsed out of body.
@property (nonatomic, copy) void (^completionHandler)(BOOL receivedChunks, NSData *body, NSInteger statusCode, NSError *error);
@end

@implementation ISHLLMStreamingResponseDelegate {
    NSMutableData *_pending;      // bytes not yet split into complete SSE lines
    NSMutableData *_body;         // whole response, kept only until streaming is confirmed
    NSInteger _statusCode;
    BOOL _receivedChunks;
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _pending = [NSMutableData data];
        _body = [NSMutableData data];
    }
    return self;
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
didReceiveResponse:(NSURLResponse *)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void) session;
    (void) dataTask;
    if ([response isKindOfClass:NSHTTPURLResponse.class])
        _statusCode = ((NSHTTPURLResponse *) response).statusCode;
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)dataTask didReceiveData:(NSData *)data {
    (void) session;
    (void) dataTask;
    [_body appendData:data];
    // Only a 200 carries an event stream; an error body is JSON to be parsed
    // whole by the caller, so don't try to read deltas out of it.
    if (_statusCode != 200)
        return;
    [_pending appendData:data];
    [self drainCompleteLines];
}

// SSE frames are line-oriented and a chunk can split mid-line, so only whole
// lines are consumed and the remainder stays buffered for the next callback.
- (void)drainCompleteLines {
    while (YES) {
        const char newline = '\n';
        NSRange lineBreak = [_pending rangeOfData:[NSData dataWithBytes:&newline length:1]
                                          options:0
                                            range:NSMakeRange(0, _pending.length)];
        if (lineBreak.location == NSNotFound)
            return;
        NSData *lineData = [_pending subdataWithRange:NSMakeRange(0, lineBreak.location)];
        [_pending replaceBytesInRange:NSMakeRange(0, NSMaxRange(lineBreak)) withBytes:NULL length:0];
        NSString *line = [[NSString alloc] initWithData:lineData encoding:NSUTF8StringEncoding];
        line = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (![line hasPrefix:@"data:"])
            continue; // comments, event: lines and blank separators
        NSString *payload = [[line substringFromIndex:5] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if ([payload isEqualToString:@"[DONE]"])
            continue; // end-of-stream marker, not a delta
        NSString *content = ISHLLMContentFromStreamingPayload(payload);
        if (content.length == 0)
            continue;
        if (!_receivedChunks) {
            _receivedChunks = YES;
            _body = nil; // streaming confirmed; stop keeping a second copy of the text
        }
        if (self.chunkHandler != nil)
            self.chunkHandler(content);
    }
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    (void) task;
    if (self.completionHandler != nil)
        self.completionHandler(_receivedChunks, _body, _statusCode, error);
    self.chunkHandler = nil;
    self.completionHandler = nil;
    // NSURLSession holds its delegate strongly until it is invalidated, so a
    // session left alive here would leak this object and, through the
    // handlers, the view controller.
    [session finishTasksAndInvalidate];
}

@end

// MARK: - Guest-shell tool support (OpenAI-compatible function calling)

typedef NS_ENUM(NSInteger, ISHLLMToolRunDecision) {
    ISHLLMToolRunDecline = 0,
    ISHLLMToolRunOnce,
    ISHLLMToolRunAllowReply, // run this and auto-run the rest of this reply
    ISHLLMToolRunAllowChat,  // run this and auto-run for the rest of the chat
};

// Bounds for the user-configurable shell-tool limits. The floor and ceiling keep
// the tool loop functional rather than being safety policy: a command with no
// timeout wedges the serial tool queue forever, and an unbounded capture blows
// out the model's context window when the result is fed back.
static const NSInteger kISHLLMToolTimeoutMinSeconds = 5;
static const NSInteger kISHLLMToolTimeoutMaxSeconds = 900;
static const NSInteger kISHLLMToolOutputMinKB = 16;
static const NSInteger kISHLLMToolOutputMaxKB = 256;
// Below the floor a multi-step task (probe environment, install a package, use
// it) can't finish before the loop bails; above the ceiling a runaway loop with
// a bad model/prompt burns too many requests before the user notices.
static const NSInteger kISHLLMToolMaxRoundsMin = 4;
static const NSInteger kISHLLMToolMaxRoundsMax = 50;

// Every request resends the entire transcript, so without some limit a
// tool-heavy conversation keeps adding another full command's output on top
// of every prior one, forever -- a handful of rounds is enough to blow past
// most models' context windows. Only the most recent tool results (as many as
// fit under a token budget) keep their full content; older ones are
// compacted down to their one-line summary instead. The budget scales with
// the model's real context window when known (see probeContextWindowIfNeeded)
// so a small local model compacts aggressively and a large one doesn't
// discard useful history it didn't need to.
static const NSInteger kISHLLMToolContextDefaultBudgetTokens = 4000; // used only when the window size is unknown
static const CGFloat kISHLLMToolContextBudgetFraction = 0.5; // fraction of a KNOWN window reserved for tool-result content

// No tokenizer is available client-side, so approximate English text at ~4
// characters/token -- a standard rule of thumb. Real tokenizers vary
// +/-30% depending on the model and content, but this is only used to size
// compaction and a status display, not anything that needs to be exact.
static NSInteger ISHLLMEstimateTokenCount(NSString *text) {
    return text.length > 0 ? (NSInteger) ((text.length + 3) / 4) : 0;
}

static NSInteger ISHLLMEstimateMessagesTokenCount(NSArray<NSDictionary<NSString *, id> *> *messages) {
    NSInteger total = 0;
    for (NSDictionary<NSString *, id> *message in messages) {
        NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
        total += ISHLLMEstimateTokenCount(content) + 4; // + rough per-message role/framing overhead
        NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : nil;
        for (NSDictionary *toolCall in toolCalls) {
            NSDictionary *function = [toolCall[@"function"] isKindOfClass:NSDictionary.class] ? toolCall[@"function"] : nil;
            NSString *arguments = [function[@"arguments"] isKindOfClass:NSString.class] ? function[@"arguments"] : @"";
            total += ISHLLMEstimateTokenCount(arguments) + 8; // + rough per-call framing overhead
        }
    }
    return total;
}

static NSString *ISHLLMFormattedTokenCount(NSInteger tokens) {
    if (tokens >= 1000)
        return [NSString stringWithFormat:@"%.1fK", tokens / 1000.0];
    return [NSString stringWithFormat:@"%ld", (long) tokens];
}

static NSInteger ISHLLMToolTimeoutSeconds(void) {
    return MAX(kISHLLMToolTimeoutMinSeconds, MIN(kISHLLMToolTimeoutMaxSeconds, UserPreferences.shared.llmToolTimeoutSeconds));
}

static NSInteger ISHLLMToolOutputLimitKB(void) {
    return MAX(kISHLLMToolOutputMinKB, MIN(kISHLLMToolOutputMaxKB, UserPreferences.shared.llmToolOutputLimitKB));
}

static NSInteger ISHLLMToolMaxRounds(void) {
    return MAX(kISHLLMToolMaxRoundsMin, MIN(kISHLLMToolMaxRoundsMax, UserPreferences.shared.llmToolMaxRounds));
}

static NSString *ISHLLMToolTimeoutTitle(NSInteger seconds) {
    if (seconds % 60 == 0 && seconds >= 60)
        return [NSString stringWithFormat:@"%ld min", (long) (seconds / 60)];
    return [NSString stringWithFormat:@"%lds", (long) seconds];
}

// The single tool exposed to the model: run a command in the iSH Linux shell and
// return its combined stdout+stderr. This makes "web search" just `curl`/`wget`
// in the environment iSH already is, with no extra API key.
static NSArray<NSDictionary<NSString *, id> *> *ISHLLMChatToolDefinitions(void) {
    return @[@{
        @"type": @"function",
        @"function": @{
            @"name": @"run_shell",
            @"description": [NSString stringWithFormat:@"Run a command in the local iSH Linux shell (/bin/sh -c) and return its combined stdout and stderr. Use this to fetch web pages or APIs, read files, or run any Linux command available in this environment. The userland varies by distro -- it may be a minimal BusyBox/Alpine system or a full Debian/Devuan/glibc one -- so use the tools that are actually present (a per-session environment note lists what was detected) and try an alternative if a command reports 'not found'. Output is capped at %ld KB and the command is killed after %ld seconds.", (long) ISHLLMToolOutputLimitKB(), (long) ISHLLMToolTimeoutSeconds()],
            @"parameters": @{
                @"type": @"object",
                @"properties": @{
                    @"command": @{
                        @"type": @"string",
                        @"description": @"The shell command line to execute, e.g. curl -fsSL 'https://wttr.in/Paris?format=3' (or wget -qO- on BusyBox systems)",
                    },
                },
                @"required": @[@"command"],
            },
        },
    }];
}

static NSString *ISHLLMToolCallID(NSDictionary *toolCall) {
    return [toolCall[@"id"] isKindOfClass:NSString.class] ? toolCall[@"id"] : nil;
}

static NSString *ISHLLMToolCallName(NSDictionary *toolCall) {
    NSDictionary *function = [toolCall[@"function"] isKindOfClass:NSDictionary.class] ? toolCall[@"function"] : nil;
    return [function[@"name"] isKindOfClass:NSString.class] ? function[@"name"] : nil;
}

// The OpenAI tool-call "arguments" field is a JSON *string*; pull the command out.
static NSString *ISHLLMToolCallCommand(NSDictionary *toolCall) {
    NSDictionary *function = [toolCall[@"function"] isKindOfClass:NSDictionary.class] ? toolCall[@"function"] : nil;
    NSString *arguments = [function[@"arguments"] isKindOfClass:NSString.class] ? function[@"arguments"] : nil;
    if (arguments.length == 0)
        return nil;
    NSData *data = [arguments dataUsingEncoding:NSUTF8StringEncoding];
    id json = data != nil ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![json isKindOfClass:NSDictionary.class])
        return nil;
    NSString *command = [json[@"command"] isKindOfClass:NSString.class] ? json[@"command"] : nil;
    if (command.length == 0)
        command = [json[@"cmd"] isKindOfClass:NSString.class] ? json[@"cmd"] : nil;
    return command.length > 0 ? command : nil;
}

// Pick out the well-formed tool calls (those with an id) from a response message.
static NSArray<NSDictionary *> *ISHLLMValidToolCalls(NSDictionary *message) {
    NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : nil;
    NSMutableArray<NSDictionary *> *valid = [NSMutableArray array];
    for (id toolCall in toolCalls) {
        if ([toolCall isKindOfClass:NSDictionary.class] && ISHLLMToolCallID(toolCall).length > 0)
            [valid addObject:toolCall];
    }
    return valid;
}

// Synchronous chat POST that works for both http (ATS-blocked, so hand-rolled
// socket) and https (NSURLSession). Blocks; call from a background queue.
static NSData *ISHLLMSynchronousChatPost(NSURL *url, NSData *body, NSString *apiKey,
                                         NSInteger *statusCodeOut, NSError **errorOut) {
    if ([url.scheme.lowercaseString isEqualToString:@"http"])
        return ISHLLMDirectHTTPPost(url, body, apiKey, statusCodeOut, errorOut);

    __block NSData *resultData = nil;
    __block NSInteger statusCode = 0;
    __block NSError *resultError = nil;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    if (apiKey.length > 0)
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
    request.HTTPBody = body;
    NSURLSessionDataTask *task = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        resultData = data;
        resultError = error;
        if ([response isKindOfClass:NSHTTPURLResponse.class])
            statusCode = ((NSHTTPURLResponse *) response).statusCode;
        dispatch_semaphore_signal(semaphore);
    }];
    [task resume];
    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
    if (statusCodeOut != NULL)
        *statusCodeOut = statusCode;
    if (errorOut != NULL)
        *errorOut = resultError;
    return resultData;
}

// Run one command in the guest and format stdout+stderr plus a status note for
// feeding back to the model. summaryOut (optional) gets a compact one-line status
// for the transcript, so the verbose output stays out of the user's view while the
// model still receives the full result. Blocks; call from a background queue (the
// primitive repoints the kernel's `current`, so it must not run on a guest task
// thread).
static NSString *ISHLLMRunGuestShellCommand(NSString *command, NSString **summaryOut) {
    NSInteger timeoutSeconds = ISHLLMToolTimeoutSeconds();
    NSInteger outputLimitKB = ISHLLMToolOutputLimitKB();
    struct guest_command_result result;
    int rc = run_guest_command_capture(command.UTF8String, NULL,
                                       (int) (timeoutSeconds * 1000), (size_t) outputLimitKB * 1024, &result);
    if (rc < 0) {
        if (summaryOut != NULL)
            *summaryOut = @"failed to start";
        return [NSString stringWithFormat:@"Could not start the command (error %d). Is the guest system booted?", rc];
    }

    NSString *captured = @"";
    if (result.output != NULL && result.output_len > 0)
        captured = [[NSString alloc] initWithBytes:result.output length:result.output_len encoding:NSUTF8StringEncoding] ?: @"";
    NSMutableArray<NSString *> *notes = [NSMutableArray array];
    if (result.timed_out)
        [notes addObject:[NSString stringWithFormat:@"timed out after %lds", (long) timeoutSeconds]];
    if (result.truncated)
        [notes addObject:[NSString stringWithFormat:@"output truncated to %ld KB", (long) outputLimitKB]];
    if (result.exited)
        [notes addObject:[NSString stringWithFormat:@"exit code %d", result.exit_code]];
    else if (result.term_signal)
        [notes addObject:[NSString stringWithFormat:@"killed by signal %d", result.term_signal]];

    if (summaryOut != NULL) {
        NSMutableArray<NSString *> *bits = [NSMutableArray array];
        if (result.exited)
            [bits addObject:[NSString stringWithFormat:@"exit %d", result.exit_code]];
        else if (result.term_signal)
            [bits addObject:[NSString stringWithFormat:@"signal %d", result.term_signal]];
        if (result.timed_out)
            [bits addObject:@"timed out"];
        [bits addObject:result.output_len > 0
            ? [NSString stringWithFormat:@"%zu bytes%@", result.output_len, result.truncated ? @"+" : @""]
            : @"no output"];
        *summaryOut = [bits componentsJoinedByString:@" · "];
    }
    free(result.output);

    NSMutableString *out = [NSMutableString stringWithString:captured.length > 0 ? captured : @"(no output)"];
    if (notes.count > 0) {
        if (![out hasSuffix:@"\n"])
            [out appendString:@"\n"];
        [out appendFormat:@"[%@]", [notes componentsJoinedByString:@", "]];
    }
    return out;
}

// Probe the guest once per chat session to learn the distro and which common
// tools are installed, so the tool guidance is distro-accurate (iSH-AOK runs
// Alpine/BusyBox, Debian/Devuan, and others). Returns a system-message note, or
// nil if the guest could not be probed. This is a fixed, read-only probe -- not a
// model-generated command -- so it runs without the per-command confirmation.
static NSString *ISHLLMDetectGuestEnvironmentNote(void) {
    const char *probe =
        ". /etc/os-release 2>/dev/null; "
        "printf 'distro=%s %s\\n' \"${NAME:-Linux}\" \"${VERSION_ID:-}\"; "
        "for t in curl wget jq python3 python git make gcc cc vi vim nano tar unzip ssh nc ss netstat ip ifconfig ps top apk apt apt-get dpkg rc-service rc-status service systemctl crontab; do "
        "command -v \"$t\" >/dev/null 2>&1 && printf 'have=%s\\n' \"$t\"; done; "
        "[ -d /etc/init.d ] && printf 'have=/etc/init.d\\n'";
    struct guest_command_result result;
    int rc = run_guest_command_capture(probe, NULL, 10000, 16 * 1024, &result);
    if (rc < 0)
        return nil;
    NSString *raw = (result.output != NULL && result.output_len > 0)
        ? ([[NSString alloc] initWithBytes:result.output length:result.output_len encoding:NSUTF8StringEncoding] ?: @"")
        : @"";
    free(result.output);
    if (raw.length == 0)
        return nil;

    NSString *distro = nil;
    NSMutableArray<NSString *> *tools = [NSMutableArray array];
    for (NSString *line in [raw componentsSeparatedByString:@"\n"]) {
        if ([line hasPrefix:@"distro="])
            distro = [[line substringFromIndex:7] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        else if ([line hasPrefix:@"have="])
            [tools addObject:[line substringFromIndex:5]];
    }

    BOOL hasCurl = [tools containsObject:@"curl"];
    BOOL hasWget = [tools containsObject:@"wget"];
    NSMutableString *note = [NSMutableString stringWithFormat:
        @"You can run shell commands in this iSH Linux guest with the run_shell tool; it returns combined stdout+stderr (capped at %ld KB, killed after %lds).",
        (long) ISHLLMToolOutputLimitKB(), (long) ISHLLMToolTimeoutSeconds()];
    if (distro.length > 0)
        [note appendFormat:@" Detected distro: %@.", distro];
    if (hasCurl && hasWget)
        [note appendString:@" Both curl and wget are installed (e.g. curl -fsSL URL or wget -qO- URL)."];
    else if (hasCurl)
        [note appendString:@" curl is installed (e.g. curl -fsSL URL); wget was not found."];
    else if (hasWget)
        [note appendString:@" wget is installed (e.g. wget -qO- URL); curl was not found."];
    else
        [note appendString:@" Neither curl nor wget was detected; use another approach for HTTP if needed."];

    // iSH-AOK never has systemd -- Alpine boots OpenRC (rc-service/rc-status),
    // Devuan boots sysvinit (service, /etc/init.d/<name>). Say this plainly so
    // the model doesn't burn a turn discovering that systemctl is absent.
    if ([tools containsObject:@"rc-service"]) {
        [note appendString:@" Init system: OpenRC. Manage services with rc-service <name> status|start|stop and see everything running with rc-status."];
    } else if ([tools containsObject:@"service"] || [tools containsObject:@"/etc/init.d"]) {
        [note appendString:@" Init system: sysvinit. Manage services with service <name> status|start|stop, or directly via /etc/init.d/<name> status|start|stop; list scripts with ls /etc/init.d."];
    } else if (![tools containsObject:@"systemctl"]) {
        [note appendString:@" No init/service manager tooling (rc-service/service/systemctl) was detected in PATH."];
    }
    if ([tools containsObject:@"apk"])
        [note appendString:@" Package manager: apk (e.g. apk info, apk add <pkg> as root)."];
    else if ([tools containsObject:@"apt"] || [tools containsObject:@"apt-get"] || [tools containsObject:@"dpkg"])
        [note appendString:@" Package manager: apt/dpkg (e.g. dpkg -l, apt list --installed, apt install <pkg> as root)."];
    if (tools.count > 0)
        [note appendFormat:@" Detected tools: %@.", [tools componentsJoinedByString:@", "]];
    [note appendString:@" This list is not exhaustive -- absence above doesn't mean a tool is missing, only that it wasn't in the fixed probe list. Use only tools you've confirmed are present; if a command reports 'not found', try an alternative or check with command -v first."];
    return note;
}

// Build the tool-loop system message: the cached distro/tool note plus a fresh
// current-time anchor. The time anchor matters because time/date questions
// otherwise lead the model to guess or reuse a stale timestamp from an earlier
// turn (especially when a network tool call fails).
static NSString *ISHLLMToolSystemNote(NSString *environmentNote) {
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
    formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.timeZone = [NSTimeZone timeZoneWithAbbreviation:@"UTC"];
    formatter.dateFormat = @"EEEE yyyy-MM-dd HH:mm:ss";
    NSString *now = [formatter stringFromDate:[NSDate date]];
    NSMutableString *note = [NSMutableString string];
    if (environmentNote.length > 0)
        [note appendString:environmentNote];
    if (note.length > 0)
        [note appendString:@" "];
    [note appendFormat:@"The current date and time is %@ UTC -- treat this as the authoritative clock and convert to other time zones from it, rather than guessing or reusing a time from an earlier message.", now];
    return note;
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

    UIFont *labelFont = [baseFont fontWithSize:MAX(11.0, baseFont.pointSize - 2.0)];
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
    copyButton.contentEdgeInsets = UIEdgeInsetsMake(2.0, 6.0, 2.0, 6.0);
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

static const CGFloat kISHLLMPromptFieldMaxHeight = 120.0;

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
    _transcriptTable.estimatedRowHeight = 60.0;
    _transcriptTable.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [_transcriptTable registerClass:ISHLLMChatMessageCell.class forCellReuseIdentifier:@"message"];
    if (@available(iOS 13.0, *))
        _transcriptTable.backgroundColor = UIColor.systemBackgroundColor;
    [self.view addSubview:_transcriptTable];

    _emptyStateLabel = [UILabel new];
    _emptyStateLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _emptyStateLabel.numberOfLines = 0;
    _emptyStateLabel.textAlignment = NSTextAlignmentCenter;
    _emptyStateLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
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
    _promptField.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
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
    if (@available(iOS 13.0, *))
        _activityIndicator = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    else
        _activityIndicator = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleGray];
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
        [_promptField.heightAnchor constraintLessThanOrEqualToConstant:kISHLLMPromptFieldMaxHeight],
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
    UIMenu *currentSection = [UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:@[rename, systemPrompt, clear, delete]];
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
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Add Destination"
                                                                  message:@"Pick a provider preset. Server URL, model and key stay editable in Settings."
                                                           preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSDictionary<NSString *, NSString *> *preset in ISHLLMProviderPresets()) {
        [alert addAction:[UIAlertAction actionWithTitle:preset[@"name"] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
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
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self anchorPopoverForAlertController:alert toSource:_destinationButton];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
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

- (void)showLLMSettings:(id)sender {
    (void) sender;
    UIViewController *settingsViewController = ISHCreateLLMSettingsViewController();
    if (self.navigationController != nil) {
        [self.navigationController pushViewController:settingsViewController animated:YES];
    } else {
        UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:settingsViewController];
        ISHConfigureLLMSettingsNavigationController(navigationController);
        [self presentViewController:navigationController animated:YES completion:nil];
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
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Save From Chat"
                                                                   message:blocks.count > 0 ? [@"Save destination: " stringByAppendingString:savePath] : [@"No fenced code blocks found in the last reply. Text in the transcript can be highlighted and copied directly, and each code block has its own Copy button. Save destination: " stringByAppendingString:savePath]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSUInteger i = 0; i < blocks.count; i++) {
        NSDictionary<NSString *, NSString *> *block = blocks[i];
        NSString *language = block[@"language"].length > 0 ? block[@"language"] : @"text";
        NSString *title = [NSString stringWithFormat:@"Save block %lu (%@)", (unsigned long) i + 1, language];
        [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            [self saveCodeBlock:block index:i + 1];
        }]];
    }
    if (blocks.count > 1) {
        [alert addAction:[UIAlertAction actionWithTitle:@"Save All Blocks" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            for (NSUInteger i = 0; i < blocks.count; i++)
                [self saveCodeBlock:blocks[i] index:i + 1];
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self anchorPopoverForAlertController:alert toSource:sender];
    [self presentViewController:alert animated:YES completion:nil];
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
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Prompt Actions" message:@"Use terminal context or saved prompt templates." preferredStyle:UIAlertControllerStyleActionSheet];
    NSArray<NSDictionary<NSString *, NSString *> *> *actions = @[
        @{@"title": @"Explain terminal output", @"instruction": @"Explain the important details in this terminal output. If there is an error, identify the likely cause."},
        @{@"title": @"Suggest fix for error", @"instruction": @"Find the most likely error in this terminal output and suggest concrete commands or edits to fix it."},
        @{@"title": @"Draft shell command", @"instruction": @"Based on this terminal context, draft the next safe shell command. Explain briefly before the command."},
    ];
    for (NSDictionary<NSString *, NSString *> *descriptor in actions) {
        [alert addAction:[UIAlertAction actionWithTitle:descriptor[@"title"] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            [self setPromptFieldText:[self terminalContextPromptWithInstruction:descriptor[@"instruction"]]];
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Load Prompt Template" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [self showPromptTemplatePickerFromSender:sender];
    }]];
    // Saving code out of the last reply used to have its own toolbar button;
    // the toolbar now spends two of its four slots on the chat and the
    // destination, so it lives here.
    [alert addAction:[UIAlertAction actionWithTitle:@"Save From Chat…" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [self showExtractActions:sender];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self anchorPopoverForAlertController:alert toSource:sender];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)showPromptTemplatePickerFromSender:(id)sender {
    NSURL *templatesURL = [ISHLLMPersistDirectoryURL() URLByAppendingPathComponent:@"llm-prompts" isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:templatesURL withIntermediateDirectories:YES attributes:nil error:nil];
    NSArray<NSURL *> *files = [NSFileManager.defaultManager contentsOfDirectoryAtURL:templatesURL includingPropertiesForKeys:nil options:0 error:nil];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Prompt Templates" message:@"Templates are text files in /AOK/persist/llm-prompts." preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSURL *fileURL in files) {
        if (fileURL.lastPathComponent.length == 0)
            continue;
        [alert addAction:[UIAlertAction actionWithTitle:fileURL.lastPathComponent style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            NSString *template = [NSString stringWithContentsOfURL:fileURL encoding:NSUTF8StringEncoding error:nil];
            if (template.length > 0)
                [self setPromptFieldText:template];
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Create Examples" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [@"Review this code for correctness, portability, and security.\n\n```\nPASTE_CODE_HERE\n```\n" writeToURL:[templatesURL URLByAppendingPathComponent:@"code-review.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [@"Turn this into a robust shell script with error handling:\n\n" writeToURL:[templatesURL URLByAppendingPathComponent:@"make-script.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [self appendRole:@"assistant" content:@"Created example prompt templates in /AOK/persist/llm-prompts."];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self anchorPopoverForAlertController:alert toSource:sender];
    [self presentViewController:alert animated:YES completion:nil];
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
    for (NSDictionary<NSString *, id> *message in _messages.reverseObjectEnumerator) {
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
    for (NSDictionary<NSString *, id> *message in _messages) {
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
                sentContent = [NSString stringWithFormat:@"(output omitted to save context: %@. Re-run the command if you need the output again.)",
                    summary.length > 0 ? summary : @"result compacted"];
            }
            [messages addObject:@{@"role": @"tool", @"tool_call_id": toolCallID, @"content": sentContent}];
            continue;
        }
        // An assistant turn that requested tools: keep tool_calls; content may be
        // empty, which is valid alongside tool_calls and must not be dropped.
        NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : nil;
        if (toolCalls.count > 0) {
            [messages addObject:@{@"role": role, @"content": content, @"tool_calls": toolCalls}];
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

// Don't show the commands or their output in the transcript -- only a small
// note that the model ran tools. The full command and output are still kept
// in the saved transcript file and sent to the model.
- (NSUInteger)commandCountForMessage:(NSDictionary<NSString *, id> *)message {
    NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : nil;
    NSUInteger commandCount = 0;
    for (NSDictionary *toolCall in toolCalls) {
        if ([toolCall isKindOfClass:NSDictionary.class] && ISHLLMToolCallCommand(toolCall).length > 0)
            commandCount++;
    }
    return commandCount;
}

- (void)refreshTranscript {
    [self recomputeVisibleMessageIndices];
    if (_emptyStateLabel != nil) {
        _emptyStateLabel.hidden = _visibleMessageIndices.count > 0;
        if (!_emptyStateLabel.hidden) {
            // Intentionally omit the server URL here -- it shows after Clear and
            // may contain a private host/IP the user doesn't want on screen.
            NSString *model = UserPreferences.shared.llmModel;
            _emptyStateLabel.text = [NSString stringWithFormat:@"%@\n\nDestination: %@%@%@",
                                      _messages.count > 0 ? @"Nothing to show yet." : @"Send a prompt to start this chat.",
                                      ISHLLMDestinationDisplayName(ISHLLMActiveDestination()),
                                      model.length > 0 ? [@"\nModel: " stringByAppendingString:model] : @"\nNo model set — pick one from the destination menu.",
                                      _sessionSystemPrompt.length > 0 ? @"\nSystem prompt set for this chat." : @""];
        }
    }
    [_transcriptTable reloadData];
    [self scrollTranscriptToBottomAnimated:NO];
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

    UIFont *baseFont = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    UIFont *codeFont = ISHLLMMonospaceFont(baseFont.pointSize - 1.0);
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

    NSUInteger commandCount = [self commandCountForMessage:message];
    NSString *caption = commandCount > 0
        ? [NSString stringWithFormat:@"(ran %lu shell command%@)", (unsigned long) commandCount, commandCount == 1 ? @"" : @"s"]
        : nil;

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
    if (apiKey.length > 0 && !ISHLLMUsesGeminiAPI())
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
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

    NSURL *url = [NSURL URLWithString:ISHLLMUsesGeminiAPI() ? ISHLLMGeminiGenerateEndpoint() : ISHLLMChatEndpoint()];
    if (url == nil) {
        [self appendLocalRole:@"assistant" content:[NSString stringWithFormat:@"Model set to %@, but the provider URL is invalid.", model]];
        return;
    }
    NSDictionary *body = ISHLLMUsesGeminiAPI()
        ? @{@"contents": @[@{@"role": @"user", @"parts": @[@{@"text": @"Reply with ok."}]}]}
        : @{
            @"model": model,
            @"messages": @[@{@"role": @"user", @"content": @"Reply with ok."}],
            @"stream": @NO,
            @"max_tokens": @1,
        };
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
    if (apiKey.length > 0 && !ISHLLMUsesGeminiAPI())
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];
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

    void (^runApprovedCommand)(void) = ^{
        typeof(self) self = weakSelf;
        if (self != nil)
            [self setStatus:@"Running command…" busy:YES];
        dispatch_async(ISHLLMGuestCommandQueue(), ^{
            NSString *summary = nil;
            NSString *output = ISHLLMRunGuestShellCommand(command, &summary);
            dispatch_async(dispatch_get_main_queue(), ^{
                recordAndComplete(output, summary);
            });
        });
    };

    if (command.length > 0) {
        NSNumber *priorDecision = _commandDecisionsThisReply[command];
        if (priorDecision != nil) {
            if (priorDecision.integerValue == ISHLLMToolRunDecline)
                recordAndComplete(@"The user declined to run this command.", @"declined by user (repeat request)");
            else
                runApprovedCommand();
            return;
        }
    }

    if (_autoRunCommandsThisChat || _autoRunCommandsThisReply) {
        runApprovedCommand();
        return;
    }

    [self setStatus:@"Waiting for approval…" busy:YES];
    [self confirmRunCommand:command completion:^(ISHLLMToolRunDecision decision) {
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        if (command.length > 0)
            self->_commandDecisionsThisReply[command] = @(decision);
        if (decision == ISHLLMToolRunDecline) {
            recordAndComplete(@"The user declined to run this command.", @"declined by user");
            return;
        }
        if (decision == ISHLLMToolRunAllowReply)
            self->_autoRunCommandsThisReply = YES;
        else if (decision == ISHLLMToolRunAllowChat)
            self->_autoRunCommandsThisChat = YES;
        runApprovedCommand();
    }];
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
            stringByAppendingString:toolsEnabled ? [@" " stringByAppendingString:ISHLLMToolSystemNote(self->_guestEnvironmentNote)] : @""];
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

    // Guest-shell tool use (OpenAI-compatible only): runs a non-streaming
    // function-calling loop so the model can run commands in the iSH shell.
    if (!ISHLLMUsesGeminiAPI() && UserPreferences.shared.llmToolsEnabled) {
        _autoRunCommandsThisReply = NO; // a new prompt re-arms confirmation for this reply
        __weak typeof(self) weakSelf = self;
        [self prepareGuestEnvironmentNoteThen:^{
            typeof(self) self = weakSelf;
            if (self != nil)
                [self runToolLoopRound:0 model:model apiKey:apiKey];
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
    if (apiKey.length > 0)
        [request setValue:[@"Bearer " stringByAppendingString:apiKey] forHTTPHeaderField:@"Authorization"];

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
    _promptField.scrollEnabled = fitSize.height > kISHLLMPromptFieldMaxHeight;
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
            NSString *note = ISHLLMDetectGuestEnvironmentNote();
            dispatch_async(dispatch_get_main_queue(), ^{
                typeof(self) self = weakSelf;
                if (self != nil && note.length > 0)
                    self->_guestEnvironmentNote = note;
            });
        });
    }
    continuation();
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
    NSMutableArray<NSDictionary<NSString *, id> *> *messages = [NSMutableArray array];
    NSString *systemNote = ISHLLMToolSystemNote(_guestEnvironmentNote);
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
    NSData *bodyData = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    if (bodyData == nil) {
        [self appendRole:@"assistant" content:@"Could not encode the request."];
        [self setSending:NO];
        return;
    }

    __weak typeof(self) weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSInteger statusCode = 0;
        NSError *error = nil;
        NSData *data = ISHLLMSynchronousChatPost(url, bodyData, apiKey, &statusCode, &error);
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self == nil)
                return;
            [self handleToolRoundData:data statusCode:statusCode error:error round:round model:model apiKey:apiKey];
        });
    });
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
        [self appendRole:@"assistant" content:finalText.length > 0 ? finalText : @"(The model returned an empty response.)"];
        [self setSending:NO];
        [self saveTranscript];
        return;
    }

    // Record the assistant turn (provider needs it paired with the tool results),
    // then execute each requested command.
    [_messages addObject:@{
        @"role": @"assistant",
        @"content": ISHLLMSanitizedAssistantContent(content),
        @"tool_calls": toolCalls,
    }];
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
    NSDictionary *toolCall = toolCalls[index];
    NSString *toolCallID = ISHLLMToolCallID(toolCall);
    NSString *name = ISHLLMToolCallName(toolCall);
    NSString *command = ISHLLMToolCallCommand(toolCall);

    __weak typeof(self) weakSelf = self;
    void (^recordResultAndContinue)(NSString *, NSString *) = ^(NSString *resultText, NSString *summary) {
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        [self->_messages addObject:@{
            @"role": @"tool",
            @"tool_call_id": toolCallID ?: @"",
            @"name": name ?: @"run_shell",
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
    };

    if (![name isEqualToString:@"run_shell"] || command.length == 0) {
        recordResultAndContinue([NSString stringWithFormat:@"Tool '%@' is not supported or the command was empty. Only run_shell with a non-empty \"command\" is available.", name ?: @"(unnamed)"], @"unsupported tool call");
        return;
    }

    void (^runApprovedCommand)(void) = ^{
        typeof(self) self = weakSelf;
        if (self != nil)
            [self setStatus:@"Running command…" busy:YES];
        dispatch_async(ISHLLMGuestCommandQueue(), ^{
            NSString *summary = nil;
            NSString *output = ISHLLMRunGuestShellCommand(command, &summary);
            dispatch_async(dispatch_get_main_queue(), ^{
                recordResultAndContinue(output, summary);
            });
        });
    };

    // Skip the per-command prompt if the user already approved auto-run for this
    // reply or for the whole chat.
    if (_autoRunCommandsThisChat || _autoRunCommandsThisReply) {
        runApprovedCommand();
        return;
    }

    [self setStatus:@"Waiting for approval…" busy:YES];
    [self confirmRunCommand:command completion:^(ISHLLMToolRunDecision decision) {
        typeof(self) self = weakSelf;
        if (self == nil)
            return;
        if (decision == ISHLLMToolRunDecline) {
            recordResultAndContinue(@"The user declined to run this command.", @"declined by user");
            return;
        }
        if (decision == ISHLLMToolRunAllowReply)
            self->_autoRunCommandsThisReply = YES;
        else if (decision == ISHLLMToolRunAllowChat)
            self->_autoRunCommandsThisChat = YES;
        runApprovedCommand();
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
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Auto-run all commands this chat?"
        message:@"Every command the model requests for the rest of this chat will run without confirmation. Content the model fetches (a web page, a file) can instruct it to run destructive commands or read private data, and nothing will stop that but the model itself. Auto-run re-arms when you clear the chat."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Run Once Instead" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) {
        completion(ISHLLMToolRunOnce);
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Allow All" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action) {
        [NSUserDefaults.standardUserDefaults setBool:YES forKey:kAutoRunWarningShownKey];
        completion(ISHLLMToolRunAllowChat);
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

- (void)confirmRunCommand:(NSString *)command completion:(void (^)(ISHLLMToolRunDecision decision))completion {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Run shell command?"
        message:[NSString stringWithFormat:@"The model wants to run this in the iSH shell:\n\n%@", command]
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Run" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        completion(ISHLLMToolRunOnce);
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Run, don't ask again this reply" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        completion(ISHLLMToolRunAllowReply);
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Run, allow all this chat" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        [self confirmAutoRunAllForChatWithCompletion:completion];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Don't Run" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) {
        completion(ISHLLMToolRunDecline);
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
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
        return [NSString stringWithFormat:@"Apple Foundation Models is an iOS/iPadOS 26+ on-device backend; no server URL or API key needed. %@ Shell Tools lets it run commands in the iSH shell, confirmed per command; the command timeout, output limit, and tool call round cap are adjustable above. %@ %@", ISHLLMAppleFoundationModelsUnavailableMessage(), thinkingNote, destinationsNote];
    return [NSString stringWithFormat:@"Use a /v1 OpenAI-compatible server, or the Gemini preset. Hosted providers require API keys.\nShell Tools lets an OpenAI-compatible model run commands in the iSH shell (web search via curl, etc.), confirmed per command; not available for Gemini. The command timeout, output limit, and tool call round cap are adjustable above.\n%@\n%@", thinkingNote, destinationsNote];
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
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
            NSUInteger count = ISHLLMDestinations().count;
            cell.textLabel.text = @"Destinations";
            cell.detailTextLabel.text = count == 1 ? @"1 saved" : [NSString stringWithFormat:@"%lu saved", (unsigned long) count];
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
            cell.textLabel.text = @"Shell Tools";
            cell.detailTextLabel.text = UserPreferences.shared.llmToolsEnabled ? @"On" : @"Off";
            cell.accessoryType = UserPreferences.shared.llmToolsEnabled ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
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
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Enable shell tools?"
        message:[NSString stringWithFormat:@"The model will be able to request shell commands that run in the iSH Linux environment — for web search via curl/wget, reading files, or running programs. You confirm each command before it runs, output is capped at %ld KB, and commands are killed after %@ (both adjustable below). Only enable this with a model and server you trust.",
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
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Command Timeout"
        message:@"A command that runs longer than this is killed and its partial output is returned to the model."
        preferredStyle:UIAlertControllerStyleActionSheet];
    NSInteger current = ISHLLMToolTimeoutSeconds();
    for (NSNumber *choice in @[@15, @30, @60, @120, @300, @600, @900]) {
        NSInteger seconds = choice.integerValue;
        NSString *title = ISHLLMToolTimeoutTitle(seconds);
        if (seconds == current)
            title = [title stringByAppendingString:@" ✓"];
        [sheet addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmToolTimeoutSeconds = seconds;
            [self.tableView reloadData];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    sheet.popoverPresentationController.sourceView = sourceView;
    sheet.popoverPresentationController.sourceRect = sourceView.bounds;
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)pickToolOutputLimitFromView:(UIView *)sourceView {
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Output Limit"
        message:@"Command output beyond this is truncated before being returned to the model. Larger limits use more of the model's context window."
        preferredStyle:UIAlertControllerStyleActionSheet];
    NSInteger current = ISHLLMToolOutputLimitKB();
    for (NSNumber *choice in @[@16, @64, @128, @256]) {
        NSInteger kb = choice.integerValue;
        NSString *title = [NSString stringWithFormat:@"%ld KB%@", (long) kb, kb == current ? @" ✓" : @""];
        [sheet addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmToolOutputLimitKB = kb;
            [self.tableView reloadData];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    sheet.popoverPresentationController.sourceView = sourceView;
    sheet.popoverPresentationController.sourceRect = sourceView.bounds;
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)pickToolMaxRoundsFromView:(UIView *)sourceView {
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Tool Call Rounds"
        message:@"A model reply that keeps calling tools without giving a final answer is stopped after this many rounds in a row, so a stuck model can't loop forever. Each round is one request to the model, so higher values let longer multi-step tasks (installing something, then using it) finish without you having to nudge it to continue."
        preferredStyle:UIAlertControllerStyleActionSheet];
    NSInteger current = ISHLLMToolMaxRounds();
    for (NSNumber *choice in @[@6, @10, @15, @20, @30, @50]) {
        NSInteger rounds = choice.integerValue;
        NSString *title = [NSString stringWithFormat:@"%ld%@", (long) rounds, rounds == current ? @" ✓" : @""];
        [sheet addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmToolMaxRounds = rounds;
            [self.tableView reloadData];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    sheet.popoverPresentationController.sourceView = sourceView;
    sheet.popoverPresentationController.sourceRect = sourceView.bounds;
    [self presentViewController:sheet animated:YES completion:nil];
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
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Choose Model"
                                                                   message:[NSString stringWithFormat:@"%lu models returned by %@", (unsigned long) models.count, ISHLLMModelsEndpoint()]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    NSUInteger limit = MIN(models.count, 80);
    for (NSUInteger i = 0; i < limit; i++) {
        NSString *model = models[i];
        NSString *title = [model isEqualToString:UserPreferences.shared.llmModel] ? [model stringByAppendingString:@"  Current"] : model;
        [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmModel = model;
            ISHLLMSyncActiveDestinationFromPreferences();
            [self.tableView reloadData];
        }]];
    }
    if (models.count > limit) {
        [alert addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Showing first %lu of %lu", (unsigned long) limit, (unsigned long) models.count]
                                                  style:UIAlertActionStyleDefault
                                                handler:nil]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self anchorPopoverForAlertController:alert toSource:sourceView];
    [self presentViewController:alert animated:YES completion:nil];
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

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
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
    UserPreferences.shared.llmProvider = preset[@"name"];
    UserPreferences.shared.llmServerURL = preset[@"url"] ?: @"";
    if (preset[@"model"].length > 0)
        UserPreferences.shared.llmModel = preset[@"model"];
    // Keep the saved destination describing the live configuration: a preset
    // pick edits the ACTIVE destination in place rather than adding one.
    ISHLLMSyncActiveDestinationFromPreferences();
    [self.navigationController popViewControllerAnimated:YES];
}

@end

#pragma mark - Chat list

static NSString *ISHLLMDestinationNameForID(NSString *destinationID) {
    for (NSDictionary<NSString *, NSString *> *destination in ISHLLMDestinations()) {
        if ([ISHLLMStringValue(destination, kISHLLMDestinationID) isEqualToString:destinationID])
            return ISHLLMDestinationDisplayName(destination);
    }
    return @"";
}

static NSString *ISHLLMRelativeDateDescription(double timestamp) {
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
@interface LLMDestinationEditorViewController : UITableViewController
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

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
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
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Provider" message:@"Fills in the server URL and a default model." preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSDictionary<NSString *, NSString *> *preset in ISHLLMProviderPresets()) {
        [alert addAction:[UIAlertAction actionWithTitle:preset[@"name"] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
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
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self anchorPopoverForAlertController:alert toSource:sourceView];
    [self presentViewController:alert animated:YES completion:nil];
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
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Add Destination" message:@"Start from a provider preset." preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSDictionary<NSString *, NSString *> *preset in ISHLLMProviderPresets()) {
        [alert addAction:[UIAlertAction actionWithTitle:preset[@"name"] style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
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
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self anchorPopoverForAlertController:alert toSource:sender];
    [self presentViewController:alert animated:YES completion:nil];
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

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
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

@implementation AboutViewController
{
    BOOL _didPresentInitialDiagnostics;
    __weak UIBarButtonItem *_modeButton;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    [self _updateUI];
    // Title and destination depend on which mode the window is currently in, and
    // the window isn't known until the view is in the hierarchy, so the title is
    // filled in by -_updateModeButton from -viewWillAppear: (GH #546).
    UIBarButtonItem *workspaceButton = [[UIBarButtonItem alloc] initWithTitle:@"Workspace"
                                                                        style:UIBarButtonItemStylePlain
                                                                       target:self
                                                                       action:@selector(toggleWorkspaceMode:)];
    _modeButton = workspaceButton;
    if (self.recoveryMode) {
        self.navigationItem.title = @"Recovery Mode";
        self.navigationItem.leftBarButtonItem = workspaceButton;
        self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Exit"
                                                                                  style:UIBarButtonItemStyleDone
                                                                                 target:self
                                                                                 action:@selector(exitRecovery:)];
    } else {
        self.navigationItem.rightBarButtonItem = workspaceButton;
    }
    _versionLabel.text = [NSString stringWithFormat:@"iSH-AOK %@ (Build %@)",
                          [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"],
                          [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleVersion"]];

    [UserPreferences.shared observe:@[@"capsLockMapping", @"fontSize", @"launchCommand", @"bootCommand", @"shouldLockSleepNanoseconds"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateUI];
        });
    }];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(_updateUI:) name:FsUpdatedNotification object:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self _updateUI];
    [self _updateModeButton];
}

// The button switches to whichever mode you are NOT in, so it is labelled with
// its destination: "Workspace" from the shell, "Shell" from the Workspace. It
// used to always say "Workspace" and only ever go one way, which left no way
// back to the shell short of force-quitting the app (GH #546).
- (void)_updateModeButton {
    UIBarButtonItem *modeButton = _modeButton;
    if (modeButton == nil)
        return;
    // -viewWillAppear: can run before the view has a window (both when Settings
    // is presented modally and when the Workspace embeds it as a tool window),
    // and a nil window reads as "not the Workspace" -- which is why this is
    // refreshed again from -viewDidAppear:, where the window is attached. Fall
    // back to the app's own window so an early call still resolves the mode
    // rather than silently guessing "shell".
    UIWindow *window = self.view.window ?: ISHActivePresentationViewController().view.window;
    modeButton.title = ISHWindowIsShowingWorkspace(window) ? @"Shell" : @"Workspace";
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    // The window is guaranteed attached by now; -viewWillAppear:'s attempt may
    // have had none to read.
    [self _updateModeButton];
    if (self.startInDiagnostics && !_didPresentInitialDiagnostics) {
        _didPresentInitialDiagnostics = YES;
        [self showDiagnostics:self.diagnosticsCell ?: self];
    }
}

- (IBAction)dismiss:(id)sender {
    [self dismissViewControllerAnimated:self completion:nil];
}

- (void)exitRecovery:(id)sender {
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"recovery"];
    exit(0);
}

- (void)showDiagnostics:(id)sender {
    UIViewController *viewController = ISHCreateDiagnosticsViewController();
    [self.navigationController pushViewController:viewController animated:YES];
}

- (void)toggleWorkspaceMode:(id)sender {
    (void) sender;
    // Switch the current scene's root between the two modes, the same way
    // launch-at-startup picks one (SceneDelegate sets window.rootViewController).
    // Asking iPadOS to activate a *separate* scene instead opened split screen on
    // some iPads and failed outright on others (Stage Manager / M4). Swapping the
    // root sidesteps all of that, and the scene's state restoration still reports
    // a workspace scene because its root is one.
    UIWindow *window = self.view.window;
    if (window == nil) {
        // No window to swap the root of (Settings presented detached). Only the
        // forward direction has a sensible fallback here.
        if (!ISHWindowIsShowingWorkspace(window))
            [self presentViewController:ISHCreateWorkspaceNavigationControllerForTool(nil) animated:YES completion:nil];
        return;
    }
    if (ISHWindowIsShowingWorkspace(window))
        ISHWindowShowSessionShell(window);
    else
        ISHWindowShowWorkspace(window);
    [self _updateModeButton];
}

- (void)_updateUI:(NSNotification *)notification {
    [self _updateUI];
}

- (void)_updateUI {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    self.disableDimmingSwitch.on = UserPreferences.shared.shouldDisableDimming;
    self.enableMulticoreSwitch.on = UserPreferences.shared.shouldEnableMulticore;
    self.enableHLESwitch.on = UserPreferences.shared.shouldEnableHLE;
    self.enableCryptoAccelSwitch.on = UserPreferences.shared.shouldEnableCryptoAccel;
    self.enablePixAccelSwitch.on = UserPreferences.shared.shouldEnablePixAccel;
    self.enableExtraLockingSwitch.on = UserPreferences.shared.shouldEnableExtraLocking;
    self.initialWindowCell.textLabel.text = @"Startup Mode";
    self.initialWindowCell.detailTextLabel.text = [self _initialWindowTitle];
    self.launchCommandField.text = [UserPreferences.shared.launchCommand componentsJoinedByString:@" "];
    self.bootCommandField.text = [UserPreferences.shared.bootCommand componentsJoinedByString:@" "];
    self.customDnsCell.textLabel.text = @"Custom DNS Servers";
    NSString *customDnsServers = [UserPreferences.shared.customDnsServers stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    self.customDnsCell.detailTextLabel.text = customDnsServers.length > 0 ? customDnsServers : @"Automatic";

    self.upgradeApkCell.userInteractionEnabled = FsNeedsRepositoryUpdate();
    self.upgradeApkLabel.enabled = FsNeedsRepositoryUpdate();
    self.upgradeApkBadge.hidden = !FsNeedsRepositoryUpdate();
    self.upgradeApkCell.accessibilityValue = FsNeedsRepositoryUpdate() ? @"Update available" : nil;
    [self.tableView reloadData];
}

- (NSInteger)_visibleStoryboardSectionCount {
    return [super numberOfSectionsInTableView:self.tableView];
}

// Appended sections live past the storyboard's static ones, in a fixed order:
// user-account section, then LLM section.
- (NSInteger)_userAccountSectionIndex {
    return [self _visibleStoryboardSectionCount];
}

- (NSInteger)_llmSectionIndex {
    return [self _visibleStoryboardSectionCount] + 1;
}

- (UITableViewCell *)_loginAsDefaultUserCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.text = @"Open Everything as Default User";
    UISwitch *enabledSwitch = [UISwitch new];
    enabledSwitch.on = UserPreferences.shared.shouldLoginAsDefaultUser;
    [enabledSwitch addTarget:self action:@selector(loginAsDefaultUserChanged:) forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = enabledSwitch;
    self.loginAsDefaultUserSwitch = enabledSwitch;
    return cell;
}

- (UITableViewCell *)_llmEnabledCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.text = @"LLM Client";
    UISwitch *enabledSwitch = [UISwitch new];
    enabledSwitch.on = UserPreferences.shared.shouldEnableLLMClient;
    [enabledSwitch addTarget:self action:@selector(llmClientEnabledChanged:) forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = enabledSwitch;
    self.llmClientSwitch = enabledSwitch;
    return cell;
}

- (UITableViewCell *)_llmSettingsCell {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.textLabel.text = @"LLM Settings";
    cell.detailTextLabel.text = UserPreferences.shared.llmModel;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == [self _llmSectionIndex]) {
        if (indexPath.row == 1) {
            UIViewController *settingsViewController = ISHCreateLLMSettingsViewController();
            if (self.navigationController != nil) {
                [self.navigationController pushViewController:settingsViewController animated:YES];
            } else {
                UINavigationController *navigationController = [[UINavigationController alloc] initWithRootViewController:settingsViewController];
                ISHConfigureLLMSettingsNavigationController(navigationController);
                [self presentViewController:navigationController animated:YES completion:nil];
            }
        }
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        return;
    }
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
    if (cell == self.sendFeedback) {
        [UIApplication openURL:@"https://github.com/emkey1/ish-AOK/issues/new"];
    } else if (cell == self.diagnosticsCell) {
        [self showDiagnostics:cell];
    } else if (cell == self.initialWindowCell) {
        [self _showInitialWindowPickerFromCell:cell];
    } else if (cell == self.openGithub) {
        [UIApplication openURL:@"https://github.com/emkey1/ish-AOK"];
    } else if (cell == self.openDiscord) {
        [UIApplication openURL:@"https://discord.gg/RkdBXHMbgc"];
    } else if (cell == self.exportContainerCell) {
        // copy the files to the app container so they can be extracted from iTunes file sharing
        NSURL *container = ContainerURL();
        NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask][0];
        [NSFileManager.defaultManager removeItemAtURL:[documents URLByAppendingPathComponent:@"roots copy"] error:nil];
        [NSFileManager.defaultManager copyItemAtURL:[container URLByAppendingPathComponent:@"roots"]
                                              toURL:[documents URLByAppendingPathComponent:@"roots copy"]
                                              error:nil];
    } else if (cell == self.resetMountsCell) {
        iosfs_clear_all_bookmarks();
    } else if (cell == self.customDnsCell) {
        [self _showCustomDnsServersEditorFromCell:cell];
    }
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (void)_showCustomDnsServersEditorFromCell:(UITableViewCell *)cell {
    (void) cell;
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Custom DNS Servers"
                                            message:@"Space- or comma-separated nameserver IPs written into the guest's /etc/resolv.conf on every refresh. Leave blank to follow this device's network-provided DNS automatically."
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = UserPreferences.shared.customDnsServers;
        textField.placeholder = @"e.g. 1.1.1.1 1.0.0.1";
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.spellCheckingType = UITextSpellCheckingTypeNo;
        textField.keyboardType = UIKeyboardTypeURL;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSString *value = [alert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"";
        UserPreferences.shared.customDnsServers = value;
        [self _updateUI];
        AppDelegate *appDelegate = (AppDelegate *) UIApplication.sharedApplication.delegate;
        if ([appDelegate isKindOfClass:AppDelegate.class]) {
            [appDelegate refreshDnsConfiguration];
        }
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (NSString *)_initialWindowPreferenceValue {
    NSString *value = [NSUserDefaults.standardUserDefaults stringForKey:kPreferenceInitialWindowKey];
    if ([value isEqualToString:ISHInitialWindowWorkspaceValue])
        return value;
    if ([value isEqualToString:ISHInitialWindowChooseFilesystemValue])
        return value;
    if ([value isEqualToString:ISHInitialWindowWaylandValue])
        return value;
    if ([value isEqualToString:@"session-shell"])
        return value;
    return @"terminal";
}

- (NSString *)_initialWindowTitle {
    if ([[self _initialWindowPreferenceValue] isEqualToString:ISHInitialWindowWorkspaceValue])
        return @"Workspace";
    if ([[self _initialWindowPreferenceValue] isEqualToString:ISHInitialWindowChooseFilesystemValue])
        return @"Choose Filesystem";
    if ([[self _initialWindowPreferenceValue] isEqualToString:ISHInitialWindowWaylandValue])
        return @"Wayland Display";
    if ([[self _initialWindowPreferenceValue] isEqualToString:@"session-shell"])
        return @"Session Shell (pts/1)";
    return @"Plain Terminal";
}

- (void)_setInitialWindowPreferenceValue:(NSString *)value {
    [NSUserDefaults.standardUserDefaults setObject:value forKey:kPreferenceInitialWindowKey];
    [self _updateUI];
}

- (void)_showInitialWindowPickerFromCell:(UITableViewCell *)cell {
    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:@"Startup Mode"
                                            message:@"Choose whether new app launches open the Workspace, the Wayland Display, show a filesystem chooser, or open a terminal."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    NSString *currentValue = [self _initialWindowPreferenceValue];
    NSString *workspaceTitle = [currentValue isEqualToString:ISHInitialWindowWorkspaceValue]
        ? @"Workspace  Current"
        : @"Workspace";
    NSString *chooseFilesystemTitle = [currentValue isEqualToString:ISHInitialWindowChooseFilesystemValue]
        ? @"Choose Filesystem  Current"
        : @"Choose Filesystem";
    NSString *waylandTitle = [currentValue isEqualToString:ISHInitialWindowWaylandValue]
        ? @"Wayland Display  Current"
        : @"Wayland Display";
    NSString *terminalTitle = [currentValue isEqualToString:@"terminal"]
        ? @"Plain Terminal  Current"
        : @"Plain Terminal";
    NSString *sessionTitle = [currentValue isEqualToString:@"session-shell"]
        ? @"Session Shell (pts/1)  Current"
        : @"Session Shell (pts/1)";

    [alert addAction:[UIAlertAction actionWithTitle:workspaceTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:ISHInitialWindowWorkspaceValue];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:waylandTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:ISHInitialWindowWaylandValue];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:chooseFilesystemTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:ISHInitialWindowChooseFilesystemValue];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:terminalTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:@"terminal"];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:sessionTitle
                                              style:UIAlertActionStyleDefault
                                            handler:^(__unused UIAlertAction *action) {
        [self _setInitialWindowPreferenceValue:@"session-shell"];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    UIPopoverPresentationController *popover = alert.popoverPresentationController;
    if (popover != nil) {
        popover.sourceView = cell;
        popover.sourceRect = cell.bounds;
    }
    [self presentViewController:alert animated:YES completion:nil];
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == [self _userAccountSectionIndex]) {
        NSString *accountName = [AppDelegate defaultUserAccountName];
        return accountName.length != 0
            ? [NSString stringWithFormat:@"When enabled, new Workspace terminals and app sessions sign in as \"%@\" (UID %d) instead of root. The Session Shell always signs in as root.",
               accountName, ISHDefaultUserAccountUID]
            : [NSString stringWithFormat:@"When enabled, new Workspace terminals and app sessions sign in as the UID %d account instead of root -- but this filesystem doesn't have one yet. The Session Shell always signs in as root.",
               ISHDefaultUserAccountUID];
    }
    if (section == [self _llmSectionIndex])
        return UserPreferences.shared.shouldEnableLLMClient
            ? @"When enabled, LLM Chat appears in Switch Terminal and Workspace menus."
            : @"Enable to show an OpenAI-compatible LLM client in terminal and Workspace menus.";
    if (section == 1) { // filesystems / upgrade
        if (!FsIsManaged()) {
            return @"The current filesystem is not managed by iSH.";
        } else if (!FsNeedsRepositoryUpdate()) {
            return [NSString stringWithFormat:@"The current filesystem is using %s, which is the latest version.", NEWEST_APK_VERSION];
        } else {
            return [NSString stringWithFormat:@"An upgrade to %s is available.", NEWEST_APK_VERSION];
        }
    }
    return [super tableView:tableView titleForFooterInSection:section];
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == [self _userAccountSectionIndex])
        return @"Default User";
    if (section == [self _llmSectionIndex])
        return @"LLM Client";
    return [super tableView:tableView titleForHeaderInSection:section];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return [self _visibleStoryboardSectionCount] + 2;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == [self _userAccountSectionIndex])
        return 1;
    if (section == [self _llmSectionIndex])
        return UserPreferences.shared.shouldEnableLLMClient ? 2 : 1;
    return [super tableView:tableView numberOfRowsInSection:section];
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == [self _userAccountSectionIndex])
        return [self _loginAsDefaultUserCell];
    if (indexPath.section == [self _llmSectionIndex]) {
        if (indexPath.row == 0)
            return [self _llmEnabledCell];
        return [self _llmSettingsCell];
    }
    return [super tableView:tableView cellForRowAtIndexPath:indexPath];
}

// iOS 27's UIKit bounds-checks the storyboard's static-section array. Our
// appended sections (user account, LLM) live at indexes >= the static section
// count, so any UITableViewController static *layout* delegate method reached
// via [super ...] for one of them indexes out of bounds -> "index N beyond
// bounds [0..N-1]" (reloadData asks the static layout for the appended
// section's heights). Older UIKit tolerated it; 27 throws. Guard these the
// same way we guard the data-source methods above, generalized to any
// section past the static ones rather than one specific index, since there
// are now two appended sections sharing this guard. heightForRow/
// indentationLevel are part of the static-cell support (proven by the
// [super ...] calls above); header/footer heights may be table defaults, so
// only forward those when super implements them.
- (BOOL)_isAppendedSection:(NSInteger)section {
    return section >= [self _visibleStoryboardSectionCount];
}

- (CGFloat)tableView:(UITableView *)tableView heightForRowAtIndexPath:(NSIndexPath *)indexPath {
    if ([self _isAppendedSection:indexPath.section])
        return 44;
    return [super tableView:tableView heightForRowAtIndexPath:indexPath];
}

- (NSInteger)tableView:(UITableView *)tableView indentationLevelForRowAtIndexPath:(NSIndexPath *)indexPath {
    if ([self _isAppendedSection:indexPath.section])
        return 0;
    return [super tableView:tableView indentationLevelForRowAtIndexPath:indexPath];
}

- (CGFloat)tableView:(UITableView *)tableView heightForHeaderInSection:(NSInteger)section {
    if ([self _isAppendedSection:section])
        return UITableViewAutomaticDimension;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView heightForHeaderInSection:section];
    return UITableViewAutomaticDimension;
}

- (CGFloat)tableView:(UITableView *)tableView heightForFooterInSection:(NSInteger)section {
    if ([self _isAppendedSection:section])
        return UITableViewAutomaticDimension;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView heightForFooterInSection:section];
    return UITableViewAutomaticDimension;
}

// Without an override here, UIKit falls back to its own default header/footer
// view synthesis (-[UITableViewDataSource tableView:viewForHeaderInSection:]),
// which hits the same static-section-array bounds check as the [super ...]
// calls above -- crashing on an appended section ("index N beyond bounds
// [0..N-1]"). Returning nil lets UITableView fall back to the plain
// title-based header/footer from titleForHeaderInSection/titleForFooterInSection.
- (UIView *)tableView:(UITableView *)tableView viewForHeaderInSection:(NSInteger)section {
    if ([self _isAppendedSection:section])
        return nil;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView viewForHeaderInSection:section];
    return nil;
}

- (UIView *)tableView:(UITableView *)tableView viewForFooterInSection:(NSInteger)section {
    if ([self _isAppendedSection:section])
        return nil;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView viewForFooterInSection:section];
    return nil;
}

- (IBAction)disableDimmingChanged:(id)sender {
    UserPreferences.shared.shouldDisableDimming = self.disableDimmingSwitch.on;
}

- (IBAction)enableCryptoAccelChanged:(id)sender {
    UserPreferences.shared.shouldEnableCryptoAccel = self.enableCryptoAccelSwitch.on;
}

- (IBAction)enablePixAccelChanged:(id)sender {
    UserPreferences.shared.shouldEnablePixAccel = self.enablePixAccelSwitch.on;
}

- (IBAction)enableHLEChanged:(id)sender {
    UserPreferences.shared.shouldEnableHLE = self.enableHLESwitch.on;
}

- (IBAction)enableMulticoreChanged:(id)sender {
    UserPreferences.shared.shouldEnableMulticore = self.enableMulticoreSwitch.on;
}

- (IBAction)enableExtraLockingChanged:(id)sender {
    UserPreferences.shared.shouldEnableExtraLocking = self.enableExtraLockingSwitch.on;
}

- (void)llmClientEnabledChanged:(UISwitch *)sender {
    UserPreferences.shared.shouldEnableLLMClient = sender.on;
    [self.tableView reloadData];
}

- (void)loginAsDefaultUserChanged:(UISwitch *)sender {
    UserPreferences.shared.shouldLoginAsDefaultUser = sender.on;
}

//- (IBAction)shouldLockSleepNanoseconds:(id)sender {
//    UserPreferences.shared.shouldLockSleepNanoseconds = self.shouldLockSleepNanosecondsSwitch.on;
//}

- (IBAction)textBoxSubmit:(id)sender {
    [sender resignFirstResponder];
}

- (IBAction)launchCommandChanged:(id)sender {
    UserPreferences.shared.launchCommand = [self.launchCommandField.text componentsSeparatedByString:@" "];
}

- (IBAction)bootCommandChanged:(id)sender {
    UserPreferences.shared.bootCommand = [self.bootCommandField.text componentsSeparatedByString:@" "];
}

@end
