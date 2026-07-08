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

@interface LLMClientViewController : UIViewController <UITextFieldDelegate>

@property (nonatomic, copy) NSString *initialPrompt;

@end

@interface LLMSettingsViewController : UITableViewController
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
@property (weak, nonatomic) IBOutlet UISwitch *enableExperimentalAmd64JitSwitch;
@property (weak, nonatomic) IBOutlet UISwitch *enableMulticoreSwitch;
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

static NSString *ISHLLMAppleFoundationModelsUnavailableMessage(void) {
    return @"Apple Foundation Models is selected, but this build cannot call it yet. The current SDK does not expose FoundationModels.framework, and the app still targets older iOS/iPadOS. When built with the iOS 26 SDK, this provider should use Apple's on-device Foundation Models backend with no server URL or API key.";
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

static BOOL ISHLLMDirectHTTPPostStreaming(NSURL *url,
                                          NSData *body,
                                          NSString *apiKey,
                                          void (^chunkHandler)(NSString *chunk),
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
    close(fd);
    return YES;
}

// MARK: - Guest-shell tool support (OpenAI-compatible function calling)

typedef NS_ENUM(NSInteger, ISHLLMToolRunDecision) {
    ISHLLMToolRunDecline = 0,
    ISHLLMToolRunOnce,
    ISHLLMToolRunAllowReply, // run this and auto-run the rest of this reply
    ISHLLMToolRunAllowChat,  // run this and auto-run for the rest of the chat
};

// The single tool exposed to the model: run a command in the iSH Linux shell and
// return its combined stdout+stderr. This makes "web search" just `curl`/`wget`
// in the environment iSH already is, with no extra API key.
static NSArray<NSDictionary<NSString *, id> *> *ISHLLMChatToolDefinitions(void) {
    return @[@{
        @"type": @"function",
        @"function": @{
            @"name": @"run_shell",
            @"description": @"Run a command in the local iSH Linux shell (/bin/sh -c) and return its combined stdout and stderr. Use this to fetch web pages or APIs, read files, or run any Linux command available in this environment. The userland varies by distro -- it may be a minimal BusyBox/Alpine system or a full Debian/Devuan/glibc one -- so use the tools that are actually present (a per-session environment note lists what was detected) and try an alternative if a command reports 'not found'. Output is capped at 64 KB and the command is killed after 30 seconds.",
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
    struct guest_command_result result;
    int rc = run_guest_command_capture(command.UTF8String, NULL, 30000, 64 * 1024, &result);
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
        [notes addObject:@"timed out after 30s"];
    if (result.truncated)
        [notes addObject:@"output truncated to 64 KB"];
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
        "for t in curl wget jq python3 python git; do "
        "command -v \"$t\" >/dev/null 2>&1 && printf 'have=%s\\n' \"$t\"; done";
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
    NSMutableString *note = [NSMutableString stringWithString:
        @"You can run shell commands in this iSH Linux guest with the run_shell tool; it returns combined stdout+stderr (capped at 64 KB, killed after 30s)."];
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
    if (tools.count > 0)
        [note appendFormat:@" Detected tools: %@.", [tools componentsJoinedByString:@", "]];
    [note appendString:@" Use only tools that are present; if a command reports 'not found', try an alternative."];
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

#pragma mark - Lightweight Markdown rendering

// A small, dependency-free Markdown -> NSAttributedString renderer for the chat
// transcript. NSAttributedString's own initWithMarkdown... is iOS 15+ and inline
// only (no headings/lists/code blocks), and the app targets iOS 12, so this
// handles the block- and inline-level constructs an LLM actually emits.

static UIFont *ISHLLMFontWithTraits(UIFont *font, UIFontDescriptorSymbolicTraits add) {
    UIFontDescriptor *base = font.fontDescriptor;
    UIFontDescriptor *desc = [base fontDescriptorWithSymbolicTraits:(base.symbolicTraits | add)];
    return desc ? [UIFont fontWithDescriptor:desc size:font.pointSize] : font;
}

static UIFont *ISHLLMMonospaceFont(CGFloat size) {
    if (@available(iOS 13.0, *))
        return [UIFont monospacedSystemFontOfSize:size weight:UIFontWeightRegular];
    return [UIFont fontWithName:@"Menlo" size:size] ?: [UIFont systemFontOfSize:size];
}

// Inline pass: emphasis (** * __ _), inline code (`), and links [text](url).
// Recurses for nested emphasis. Underscore emphasis is suppressed intra-word so
// snake_case and __dunder__ identifiers survive untouched.
static void ISHLLMAppendInlineMarkdown(NSMutableAttributedString *out, NSString *text, UIFont *font, NSDictionary *ctx) {
    UIColor *color = ctx[@"color"];
    UIFont *codeFont = ctx[@"codeFont"];
    UIColor *codeColor = ctx[@"codeColor"];
    UIColor *codeBg = ctx[@"codeBg"];
    UIColor *linkColor = ctx[@"linkColor"];
    NSCharacterSet *alnum = NSCharacterSet.alphanumericCharacterSet;
    NSUInteger n = text.length;
    __block NSUInteger plainStart = 0;
    void (^flushPlain)(NSUInteger) = ^(NSUInteger end) {
        if (end > plainStart) {
            NSString *chunk = [text substringWithRange:NSMakeRange(plainStart, end - plainStart)];
            [out appendAttributedString:[[NSAttributedString alloc] initWithString:chunk attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: color}]];
        }
    };
    NSUInteger i = 0;
    while (i < n) {
        unichar c = [text characterAtIndex:i];

        // inline code `...`
        if (c == '`') {
            NSRange close = [text rangeOfString:@"`" options:0 range:NSMakeRange(i + 1, n - (i + 1))];
            if (close.location != NSNotFound && close.location > i + 1) {
                flushPlain(i);
                NSString *code = [text substringWithRange:NSMakeRange(i + 1, close.location - (i + 1))];
                NSMutableDictionary *attrs = [@{NSFontAttributeName: codeFont, NSForegroundColorAttributeName: codeColor} mutableCopy];
                if (codeBg) attrs[NSBackgroundColorAttributeName] = codeBg;
                [out appendAttributedString:[[NSAttributedString alloc] initWithString:code attributes:attrs]];
                i = close.location + 1; plainStart = i; continue;
            }
        }

        // bold **...** or __...__
        if ((c == '*' || c == '_') && i + 1 < n && [text characterAtIndex:i + 1] == c) {
            BOOL underscore = (c == '_');
            BOOL boundaryOK = !(underscore && i > 0 && [alnum characterIsMember:[text characterAtIndex:i - 1]]);
            NSRange close = [text rangeOfString:(underscore ? @"__" : @"**") options:0 range:NSMakeRange(i + 2, n - (i + 2))];
            if (boundaryOK && close.location != NSNotFound && close.location > i + 2) {
                NSUInteger after = close.location + 2;
                if (!(underscore && after < n && [alnum characterIsMember:[text characterAtIndex:after]])) {
                    flushPlain(i);
                    NSString *inner = [text substringWithRange:NSMakeRange(i + 2, close.location - (i + 2))];
                    ISHLLMAppendInlineMarkdown(out, inner, ISHLLMFontWithTraits(font, UIFontDescriptorTraitBold), ctx);
                    i = close.location + 2; plainStart = i; continue;
                }
            }
        }

        // italic *...* or _..._
        if (c == '*' || c == '_') {
            BOOL underscore = (c == '_');
            BOOL boundaryOK = !(underscore && i > 0 && [alnum characterIsMember:[text characterAtIndex:i - 1]]);
            if (boundaryOK && i + 1 < n && [text characterAtIndex:i + 1] != ' ') {
                NSRange close = [text rangeOfString:(underscore ? @"_" : @"*") options:0 range:NSMakeRange(i + 1, n - (i + 1))];
                if (close.location != NSNotFound && close.location > i + 1) {
                    NSUInteger after = close.location + 1;
                    if (!(underscore && after < n && [alnum characterIsMember:[text characterAtIndex:after]])) {
                        flushPlain(i);
                        NSString *inner = [text substringWithRange:NSMakeRange(i + 1, close.location - (i + 1))];
                        ISHLLMAppendInlineMarkdown(out, inner, ISHLLMFontWithTraits(font, UIFontDescriptorTraitItalic), ctx);
                        i = close.location + 1; plainStart = i; continue;
                    }
                }
            }
        }

        // link [label](url)
        if (c == '[') {
            NSRange closeBracket = [text rangeOfString:@"]" options:0 range:NSMakeRange(i + 1, n - (i + 1))];
            if (closeBracket.location != NSNotFound && closeBracket.location + 1 < n && [text characterAtIndex:closeBracket.location + 1] == '(') {
                NSUInteger urlStart = closeBracket.location + 2;
                NSRange closeParen = [text rangeOfString:@")" options:0 range:NSMakeRange(urlStart, n - urlStart)];
                if (closeParen.location != NSNotFound) {
                    flushPlain(i);
                    NSString *label = [text substringWithRange:NSMakeRange(i + 1, closeBracket.location - (i + 1))];
                    NSString *urlString = [[text substringWithRange:NSMakeRange(urlStart, closeParen.location - urlStart)] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
                    NSMutableDictionary *attrs = [@{NSFontAttributeName: font, NSForegroundColorAttributeName: linkColor, NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle)} mutableCopy];
                    NSURL *url = [NSURL URLWithString:urlString];
                    if (url) attrs[NSLinkAttributeName] = url;
                    [out appendAttributedString:[[NSAttributedString alloc] initWithString:(label.length ? label : urlString) attributes:attrs]];
                    i = closeParen.location + 1; plainStart = i; continue;
                }
            }
        }

        i++;
    }
    flushPlain(n);
}

static NSAttributedString *ISHLLMAttributedStringFromMarkdown(NSString *markdown, UIFont *baseFont, UIColor *textColor, UIColor *secondaryColor, UIColor *codeBg, UIColor *linkColor) {
    NSMutableAttributedString *out = [NSMutableAttributedString new];
    if (markdown.length == 0)
        return out;
    CGFloat baseSize = baseFont.pointSize;
    UIFont *codeFont = ISHLLMMonospaceFont(baseSize - 1.0);
    NSDictionary *ctx = @{@"color": textColor, @"codeFont": codeFont, @"codeColor": textColor, @"codeBg": codeBg, @"linkColor": linkColor};
    NSCharacterSet *ws = NSCharacterSet.whitespaceCharacterSet;

    NSMutableParagraphStyle *bodyStyle = [NSMutableParagraphStyle new];
    bodyStyle.paragraphSpacing = baseSize * 0.35;
    NSMutableParagraphStyle *codeStyle = [NSMutableParagraphStyle new];
    codeStyle.firstLineHeadIndent = 8.0; codeStyle.headIndent = 8.0; codeStyle.paragraphSpacing = baseSize * 0.35;

    void (^appendNewline)(void) = ^{
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:@{NSFontAttributeName: baseFont, NSForegroundColorAttributeName: textColor}]];
    };

    NSArray<NSString *> *lines = [markdown componentsSeparatedByString:@"\n"];
    BOOL inFence = NO;
    NSMutableArray<NSString *> *codeLines = [NSMutableArray array];
    void (^flushCode)(void) = ^{
        NSString *code = [codeLines componentsJoinedByString:@"\n"];
        NSMutableDictionary *attrs = [@{NSFontAttributeName: codeFont, NSForegroundColorAttributeName: textColor, NSParagraphStyleAttributeName: codeStyle} mutableCopy];
        if (codeBg) attrs[NSBackgroundColorAttributeName] = codeBg;
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:code attributes:attrs]];
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n" attributes:@{NSFontAttributeName: codeFont}]];
        [codeLines removeAllObjects];
    };

    for (NSString *rawLine in lines) {
        NSString *trimmed = [rawLine stringByTrimmingCharactersInSet:ws];

        // fenced code block ``` or ~~~
        if ([trimmed hasPrefix:@"```"] || [trimmed hasPrefix:@"~~~"]) {
            if (inFence) { flushCode(); inFence = NO; } else { inFence = YES; }
            continue;
        }
        if (inFence) { [codeLines addObject:rawLine]; continue; }

        if (trimmed.length == 0) { appendNewline(); continue; }

        // horizontal rule
        if ([trimmed isEqualToString:@"---"] || [trimmed isEqualToString:@"***"] || [trimmed isEqualToString:@"___"] ||
            [trimmed isEqualToString:@"- - -"] || [trimmed isEqualToString:@"* * *"]) {
            [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"————————\n" attributes:@{NSFontAttributeName: baseFont, NSForegroundColorAttributeName: secondaryColor}]];
            continue;
        }

        // ATX heading # .. ######
        NSUInteger hashes = 0;
        while (hashes < trimmed.length && [trimmed characterAtIndex:hashes] == '#') hashes++;
        if (hashes >= 1 && hashes <= 6 && hashes < trimmed.length && [trimmed characterAtIndex:hashes] == ' ') {
            NSString *headingText = [[trimmed substringFromIndex:hashes] stringByTrimmingCharactersInSet:ws];
            CGFloat scale = hashes == 1 ? 1.5 : hashes == 2 ? 1.3 : hashes == 3 ? 1.15 : 1.05;
            UIFont *hFont = ISHLLMFontWithTraits([baseFont fontWithSize:(CGFloat)round(baseSize * scale)], UIFontDescriptorTraitBold);
            NSUInteger start = out.length;
            ISHLLMAppendInlineMarkdown(out, headingText, hFont, ctx);
            [out addAttribute:NSParagraphStyleAttributeName value:bodyStyle range:NSMakeRange(start, out.length - start)];
            appendNewline();
            continue;
        }

        // blockquote
        if ([trimmed hasPrefix:@">"]) {
            NSString *quote = [[trimmed substringFromIndex:1] stringByTrimmingCharactersInSet:ws];
            NSMutableParagraphStyle *qs = [NSMutableParagraphStyle new];
            qs.firstLineHeadIndent = 12.0; qs.headIndent = 12.0; qs.paragraphSpacing = baseSize * 0.35;
            NSMutableDictionary *qctx = [ctx mutableCopy]; qctx[@"color"] = secondaryColor;
            NSUInteger start = out.length;
            ISHLLMAppendInlineMarkdown(out, quote, ISHLLMFontWithTraits(baseFont, UIFontDescriptorTraitItalic), qctx);
            [out addAttribute:NSParagraphStyleAttributeName value:qs range:NSMakeRange(start, out.length - start)];
            appendNewline();
            continue;
        }

        // list items (bullet or numbered), with leading-space nesting
        NSUInteger lead = 0;
        while (lead < rawLine.length && [rawLine characterAtIndex:lead] == ' ') lead++;
        NSString *body = [rawLine substringFromIndex:lead];
        unichar first = body.length ? [body characterAtIndex:0] : 0;
        BOOL isBullet = (first == '-' || first == '*' || first == '+') && body.length > 1 && [body characterAtIndex:1] == ' ';
        NSUInteger digits = 0;
        while (digits < body.length && [body characterAtIndex:digits] >= '0' && [body characterAtIndex:digits] <= '9') digits++;
        BOOL isNumbered = digits > 0 && digits + 1 < body.length && [body characterAtIndex:digits] == '.' && [body characterAtIndex:digits + 1] == ' ';
        if (isBullet || isNumbered) {
            NSString *marker = isBullet ? @"•\t" : [NSString stringWithFormat:@"%@.\t", [body substringToIndex:digits]];
            NSString *item = [[body substringFromIndex:(isBullet ? 2 : digits + 2)] stringByTrimmingCharactersInSet:ws];
            CGFloat indent = (isBullet ? 16.0 : 22.0) + (lead / 2) * 14.0;
            NSMutableParagraphStyle *ls = [NSMutableParagraphStyle new];
            ls.headIndent = indent; ls.firstLineHeadIndent = indent - (isBullet ? 14.0 : 18.0);
            ls.paragraphSpacing = baseSize * 0.2;
            ls.tabStops = @[[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:indent options:@{}]];
            NSUInteger start = out.length;
            [out appendAttributedString:[[NSAttributedString alloc] initWithString:marker attributes:@{NSFontAttributeName: (isNumbered ? ISHLLMFontWithTraits(baseFont, UIFontDescriptorTraitBold) : baseFont), NSForegroundColorAttributeName: textColor}]];
            ISHLLMAppendInlineMarkdown(out, item, baseFont, ctx);
            [out addAttribute:NSParagraphStyleAttributeName value:ls range:NSMakeRange(start, out.length - start)];
            appendNewline();
            continue;
        }

        // default paragraph line
        NSUInteger start = out.length;
        ISHLLMAppendInlineMarkdown(out, rawLine, baseFont, ctx);
        [out addAttribute:NSParagraphStyleAttributeName value:bodyStyle range:NSMakeRange(start, out.length - start)];
        appendNewline();
    }
    if (inFence && codeLines.count > 0)
        flushCode(); // unterminated fence (e.g. still streaming)

    return out;
}

@implementation LLMClientViewController {
    UIStackView *_toolbarStackView;
    UITextView *_transcriptView;
    UITextField *_promptField;
    UIButton *_sendButton;
    NSMutableArray<NSDictionary<NSString *, id> *> *_messages;
    NSURLSessionDataTask *_activeTask;
    BOOL _autoRunCommandsThisReply; // skip per-command confirm for the current reply
    BOOL _autoRunCommandsThisChat;  // skip per-command confirm until the chat is cleared
    NSString *_guestEnvironmentNote; // cached distro/tool probe for the tool system prompt
    UILabel *_statusLabel;
    UIActivityIndicatorView *_activityIndicator;
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
    NSArray *storedMessages = [NSArray arrayWithContentsOfURL:ISHLLMTranscriptURL()];
    if ([storedMessages isKindOfClass:NSArray.class]) {
        for (id message in storedMessages) {
            if ([message isKindOfClass:NSDictionary.class] && [message[@"role"] isKindOfClass:NSString.class] && [message[@"content"] isKindOfClass:NSString.class])
                [_messages addObject:message];
        }
    }

    _transcriptView = [[UITextView alloc] initWithFrame:CGRectZero];
    _transcriptView.translatesAutoresizingMaskIntoConstraints = NO;
    _transcriptView.editable = NO;
    _transcriptView.alwaysBounceVertical = YES;
    _transcriptView.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    if (@available(iOS 13.0, *)) {
        _transcriptView.backgroundColor = UIColor.systemBackgroundColor;
        _transcriptView.textColor = UIColor.labelColor;
    }
    [self.view addSubview:_transcriptView];

    UIView *inputBar = [UIView new];
    inputBar.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:inputBar];

    _promptField = [UITextField new];
    _promptField.translatesAutoresizingMaskIntoConstraints = NO;
    _promptField.borderStyle = UITextBorderStyleRoundedRect;
    _promptField.placeholder = @"Ask the configured model";
    _promptField.returnKeyType = UIReturnKeySend;
    _promptField.autocorrectionType = UITextAutocorrectionTypeDefault;
    _promptField.delegate = self;
    _promptField.accessibilityLabel = @"Prompt input";
    [inputBar addSubview:_promptField];

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
    NSArray<NSDictionary<NSString *, NSString *> *> *toolbarButtons = @[
        @{@"title": @"Settings", @"selector": NSStringFromSelector(@selector(showLLMSettings:))},
        @{@"title": @"Actions", @"selector": NSStringFromSelector(@selector(showPromptActions:))},
        @{@"title": @"Save", @"selector": NSStringFromSelector(@selector(showExtractActions:))},
        @{@"title": @"Clear", @"selector": NSStringFromSelector(@selector(clearTranscript:))},
    ];
    for (NSDictionary<NSString *, NSString *> *descriptor in toolbarButtons) {
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
        button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
        [button setTitle:descriptor[@"title"] forState:UIControlStateNormal];
        [button addTarget:self action:NSSelectorFromString(descriptor[@"selector"]) forControlEvents:UIControlEventTouchUpInside];
        [_toolbarStackView addArrangedSubview:button];
    }

    self.navigationItem.rightBarButtonItems = @[
        [[UIBarButtonItem alloc] initWithTitle:@"Settings" style:UIBarButtonItemStylePlain target:self action:@selector(showLLMSettings:)],
        [[UIBarButtonItem alloc] initWithTitle:@"Actions" style:UIBarButtonItemStylePlain target:self action:@selector(showPromptActions:)],
        [[UIBarButtonItem alloc] initWithTitle:@"Save" style:UIBarButtonItemStylePlain target:self action:@selector(showExtractActions:)],
        [[UIBarButtonItem alloc] initWithTitle:@"Clear" style:UIBarButtonItemStylePlain target:self action:@selector(clearTranscript:)],
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

    UILayoutGuide *safeArea = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_toolbarStackView.topAnchor constraintEqualToAnchor:safeArea.topAnchor constant:6.0],
        [_toolbarStackView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:10.0],
        [_toolbarStackView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-10.0],
        [_toolbarStackView.heightAnchor constraintGreaterThanOrEqualToConstant:32.0],

        [_transcriptView.topAnchor constraintEqualToAnchor:_toolbarStackView.bottomAnchor constant:4.0],
        [_transcriptView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor],
        [_transcriptView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor],
        [_transcriptView.bottomAnchor constraintEqualToAnchor:statusRow.topAnchor constant:-2.0],

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
        [_sendButton.leadingAnchor constraintEqualToAnchor:_promptField.trailingAnchor constant:8.0],
        [_sendButton.trailingAnchor constraintEqualToAnchor:inputBar.trailingAnchor],
        [_sendButton.centerYAnchor constraintEqualToAnchor:_promptField.centerYAnchor],
        [_sendButton.widthAnchor constraintEqualToConstant:56.0],
    ]];

    [self refreshTranscript];
    [self setStatus:[self idleStatusText] busy:NO];
    if (self.initialPrompt.length > 0)
        _promptField.text = self.initialPrompt;
}

- (void)dealloc {
    [_activeTask cancel];
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

- (void)clearTranscript:(id)sender {
    (void) sender;
    [_messages removeAllObjects];
    _autoRunCommandsThisChat = NO; // a fresh chat re-arms per-command confirmation
    [self saveTranscript];
    [self refreshTranscript];
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
    NSString *selectedText = [self selectedTranscriptText];
    NSString *savePath = @"/AOK/persist/llm-extracts";
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Save From Chat"
                                                                   message:(blocks.count > 0 || selectedText.length > 0) ? [@"Save destination: " stringByAppendingString:savePath] : [@"No highlighted text or fenced code blocks found. Save destination: " stringByAppendingString:savePath]
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    if (selectedText.length > 0) {
        [alert addAction:[UIAlertAction actionWithTitle:@"Save Highlighted Text" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            [self saveExtractText:selectedText extension:@"txt" label:@"highlighted"];
        }]];
    }
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

- (NSString *)selectedTranscriptText {
    NSRange selectedRange = _transcriptView.selectedRange;
    if (selectedRange.length == 0 || NSMaxRange(selectedRange) > _transcriptView.text.length)
        return @"";
    return [_transcriptView.text substringWithRange:selectedRange];
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
            self->_promptField.text = [self terminalContextPromptWithInstruction:descriptor[@"instruction"]];
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Load Prompt Template" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        [self showPromptTemplatePickerFromSender:sender];
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
                self->_promptField.text = template;
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
    NSMutableArray<NSDictionary<NSString *, id> *> *messages = [NSMutableArray array];
    for (NSDictionary<NSString *, id> *message in _messages) {
        if ([self messageIsLocalOnly:message])
            continue;
        NSString *role = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : nil;
        if (role.length == 0)
            continue;
        NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
        // A tool result: the model needs the matching tool_call_id to thread it.
        if ([role isEqualToString:@"tool"]) {
            NSString *toolCallID = [message[@"tool_call_id"] isKindOfClass:NSString.class] ? message[@"tool_call_id"] : nil;
            if (toolCallID.length > 0)
                [messages addObject:@{@"role": @"tool", @"tool_call_id": toolCallID, @"content": content}];
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

- (void)saveTranscript {
    NSURL *directoryURL = ISHLLMPersistDirectoryURL();
    if (directoryURL != nil)
        [NSFileManager.defaultManager createDirectoryAtURL:directoryURL withIntermediateDirectories:YES attributes:nil error:nil];
    [_messages writeToURL:ISHLLMTranscriptURL() atomically:YES];
}

- (void)refreshTranscript {
    UIFont *baseFont = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    UIColor *textColor = UIColor.blackColor;
    UIColor *secondaryColor = UIColor.grayColor;
    UIColor *codeBg = [UIColor colorWithWhite:0.0 alpha:0.06];
    UIColor *linkColor = [UIColor colorWithRed:0.0 green:0.48 blue:1.0 alpha:1.0];
    if (@available(iOS 13.0, *)) {
        textColor = UIColor.labelColor;
        secondaryColor = UIColor.secondaryLabelColor;
        codeBg = UIColor.tertiarySystemFillColor; // adapts to light/dark
        linkColor = UIColor.linkColor;
    }
    NSDictionary *plainAttrs = @{NSFontAttributeName: baseFont, NSForegroundColorAttributeName: textColor};
    NSDictionary *roleAttrs = @{NSFontAttributeName: ISHLLMFontWithTraits(baseFont, UIFontDescriptorTraitBold), NSForegroundColorAttributeName: secondaryColor};
    NSDictionary *noteAttrs = @{NSFontAttributeName: ISHLLMFontWithTraits([baseFont fontWithSize:baseFont.pointSize - 1.0], UIFontDescriptorTraitItalic), NSForegroundColorAttributeName: secondaryColor};

    NSMutableAttributedString *out = [NSMutableAttributedString new];
    for (NSDictionary<NSString *, id> *message in _messages) {
        NSString *messageRole = [message[@"role"] isKindOfClass:NSString.class] ? message[@"role"] : @"";
        NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
        if ([messageRole isEqualToString:@"tool"])
            continue; // tool output is hidden from the transcript (the model still gets it)
        BOOL isAssistant = [messageRole isEqualToString:@"assistant"];
        if (content.length > 0) {
            // Bold role header on its own line, then the message body. Assistant
            // replies are rendered as Markdown; the user's own prompt is shown
            // verbatim so their literal text is never reinterpreted.
            [out appendAttributedString:[[NSAttributedString alloc] initWithString:(isAssistant ? @"Assistant\n" : @"You\n") attributes:roleAttrs]];
            if (isAssistant)
                [out appendAttributedString:ISHLLMAttributedStringFromMarkdown(content, baseFont, textColor, secondaryColor, codeBg, linkColor)];
            else
                [out appendAttributedString:[[NSAttributedString alloc] initWithString:content attributes:plainAttrs]];
            [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n\n" attributes:plainAttrs]];
        }
        // Don't show the commands or their output in the transcript -- only a small
        // note that the model ran tools. The full command and output are still kept
        // in the saved transcript file and sent to the model.
        NSArray *toolCalls = [message[@"tool_calls"] isKindOfClass:NSArray.class] ? message[@"tool_calls"] : nil;
        NSUInteger commandCount = 0;
        for (NSDictionary *toolCall in toolCalls) {
            if ([toolCall isKindOfClass:NSDictionary.class] && ISHLLMToolCallCommand(toolCall).length > 0)
                commandCount++;
        }
        if (commandCount > 0)
            [out appendAttributedString:[[NSAttributedString alloc] initWithString:[NSString stringWithFormat:@"(ran %lu shell command%@)\n\n", (unsigned long) commandCount, commandCount == 1 ? @"" : @"s"] attributes:noteAttrs]];
    }
    if (out.length == 0) {
        // Intentionally omit the server URL here -- it shows after Clear and may
        // contain a private host/IP the user doesn't want on screen.
        NSString *hint = [NSString stringWithFormat:@"Configure an OpenAI-compatible server in Settings, then send a prompt.\n\nModel: %@\n",
                          UserPreferences.shared.llmModel];
        [out appendAttributedString:[[NSAttributedString alloc] initWithString:hint attributes:plainAttrs]];
    }
    _transcriptView.attributedText = out;
    NSRange bottom = NSMakeRange(out.length, 0);
    [_transcriptView scrollRangeToVisible:bottom];
}

- (void)setSending:(BOOL)sending {
    _sendButton.enabled = !sending;
    _promptField.enabled = !sending;
    [_sendButton setTitle:(sending ? @"..." : @"Send") forState:UIControlStateNormal];
    _sendButton.accessibilityLabel = sending ? @"Sending" : @"Send";
    if (sending)
        [self setStatus:@"Working…" busy:YES];
    else
        [self setStatus:[self idleStatusText] busy:NO];
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

- (NSString *)idleStatusText {
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels)
        return @"Apple Foundation Models";
    NSString *model = [UserPreferences.shared.llmModel stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (model.length == 0)
        return @"Set a model in Settings";
    return [NSString stringWithFormat:@"Ready · %@", model];
}

- (void)handleLLMResponseData:(NSData *)data response:(NSURLResponse *)response error:(NSError *)error {
    [self setSending:NO];
    _activeTask = nil;
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

- (void)appendStreamingAssistantChunk:(NSString *)chunk toMessageAtIndex:(NSUInteger)index {
    if (index >= _messages.count || chunk.length == 0)
        return;
    NSMutableDictionary<NSString *, NSString *> *message = [_messages[index] mutableCopy];
    NSString *content = ISHLLMStreamingAssistantContent([message[@"content"] ?: @"" stringByAppendingString:chunk]);
    message[@"content"] = content;
    _messages[index] = message;
    [self refreshTranscript];
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
    [self refreshTranscript];
}

- (void)appendModelListFromData:(NSData *)data statusCode:(NSInteger)statusCode error:(NSError *)error {
    [self setSending:NO];
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
    __weak typeof(self) weakSelf = self;
    _activeTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self != nil)
                [self appendModelListFromData:data statusCode:http.statusCode error:error];
        });
    }];
    [_activeTask resume];
}

- (void)appendModelLoadResultWithModel:(NSString *)model data:(NSData *)data statusCode:(NSInteger)statusCode error:(NSError *)error {
    [self setSending:NO];
    _activeTask = nil;
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
    _activeTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *) response : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self != nil)
                [self appendModelLoadResultWithModel:model data:data statusCode:http.statusCode error:error];
        });
    }];
    [_activeTask resume];
}

- (void)sendPrompt:(id)sender {
    (void) sender;
    NSString *prompt = [_promptField.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (prompt.length == 0)
        return;
    if ([prompt isEqualToString:@"/models"]) {
        _promptField.text = @"";
        [self appendLocalRole:@"user" content:prompt];
        [self queryModelsInTranscript];
        return;
    }
    if ([prompt isEqualToString:@"/model"] || [prompt hasPrefix:@"/model "]) {
        _promptField.text = @"";
        [self appendLocalRole:@"user" content:prompt];
        [self setAndLoadModelFromCommand:prompt];
        return;
    }
    if (ISHLLMCurrentBackend() == AOKLLMBackendAppleFoundationModels) {
        _promptField.text = @"";
        [self appendLocalRole:@"user" content:prompt];
        [self appendLocalRole:@"assistant" content:ISHLLMAppleFoundationModelsUnavailableMessage()];
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

    _promptField.text = @"";
    [self appendRole:@"user" content:prompt];
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
            NSString *role = [message[@"role"] isEqualToString:@"assistant"] ? @"model" : @"user";
            NSString *content = [message[@"content"] isKindOfClass:NSString.class] ? message[@"content"] : @"";
            if (content.length > 0)
                [contents addObject:@{@"role": role, @"parts": @[@{@"text": content}]}];
        }
        NSDictionary *body = @{@"contents": contents};
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

    BOOL useDirectHTTP = [[url.scheme lowercaseString] isEqualToString:@"http"];
    NSDictionary *body = @{
        @"model": model,
        @"messages": [self providerMessages],
        @"stream": @(useDirectHTTP),
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
            }, &statusCode, &directError);
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

    __weak typeof(self) weakSelf = self;
    _activeTask = [NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self == nil)
                return;
            [self handleLLMResponseData:data response:response error:error];
        });
    }];
    [_activeTask resume];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    (void) textField;
    [self sendPrompt:nil];
    return YES;
}

// MARK: - Guest-shell tool loop
//
// One "round" = one non-streaming chat request that advertises the run_shell
// tool. If the model answers with tool calls we run each command in the guest,
// append the results as `tool` messages, and start another round; otherwise the
// round's content is the final answer. Bounded by kISHLLMMaxToolRounds.

static const NSInteger kISHLLMMaxToolRounds = 6;

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

- (void)runToolLoopRound:(NSInteger)round model:(NSString *)model apiKey:(NSString *)apiKey {
    NSURL *url = [NSURL URLWithString:ISHLLMChatEndpoint()];
    if (url == nil) {
        [self appendRole:@"assistant" content:@"Invalid LLM server URL."];
        [self setSending:NO];
        return;
    }
    if (round >= kISHLLMMaxToolRounds) {
        [self appendRole:@"assistant" content:@"Stopped after too many tool calls in a row. Send another message to continue."];
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
        completion(ISHLLMToolRunAllowChat);
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Don't Run" style:UIAlertActionStyleCancel handler:^(UIAlertAction *action) {
        completion(ISHLLMToolRunDecline);
    }]];
    [[self ish_presentationViewController] presentViewController:alert animated:YES completion:nil];
}

@end

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
    return 8;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void) tableView;
    if (section == 0)
        return nil;
    if (ISHLLMUsesAppleFoundationModels())
        return @"Apple Foundation Models is an iOS/iPadOS 26+ on-device backend. This build exposes the provider setting, but runtime calls require building with an SDK that includes FoundationModels.framework. Chat history is saved in /AOK/persist/llm-chat.json.";
    return @"Use a /v1 OpenAI-compatible server, or the Gemini preset. Hosted providers require API keys.\nShell Tools lets an OpenAI-compatible model run commands in the iSH shell (web search via curl, etc.), confirmed per command; not available for Gemini.\nChat history is saved in /AOK/persist/llm-chat.json.";
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
    if (indexPath.row == 0) {
        cell.textLabel.text = @"Provider";
        cell.detailTextLabel.text = UserPreferences.shared.llmProvider;
    } else if (indexPath.row == 1) {
        cell.textLabel.text = @"Server URL";
        cell.detailTextLabel.text = ISHLLMUsesAppleFoundationModels() ? @"On-device" : UserPreferences.shared.llmServerURL;
        if (ISHLLMUsesAppleFoundationModels())
            cell.accessoryType = UITableViewCellAccessoryNone;
    } else if (indexPath.row == 2) {
        cell.textLabel.text = @"Model";
        cell.detailTextLabel.text = UserPreferences.shared.llmModel;
    } else if (indexPath.row == 3) {
        cell.textLabel.text = @"API Format";
        cell.detailTextLabel.text = ISHLLMCurrentAPIFormat();
        cell.accessoryType = UITableViewCellAccessoryNone;
    } else if (indexPath.row == 4) {
        cell.textLabel.text = @"API Key";
        cell.detailTextLabel.text = ISHLLMUsesAppleFoundationModels() ? @"Not used" : (UserPreferences.shared.llmAPIKey.length > 0 ? @"Set" : (ISHLLMProviderRequiresAPIKey() ? @"Required" : @"Optional"));
        if (ISHLLMUsesAppleFoundationModels())
            cell.accessoryType = UITableViewCellAccessoryNone;
    } else if (indexPath.row == 5) {
        cell.textLabel.text = @"Query Models";
        cell.detailTextLabel.text = @"/models";
        cell.accessoryType = UITableViewCellAccessoryNone;
    } else if (indexPath.row == 6) {
        cell.textLabel.text = @"Test Connection";
        cell.detailTextLabel.text = @"";
        cell.accessoryType = UITableViewCellAccessoryNone;
    } else {
        cell.textLabel.text = @"Shell Tools";
        cell.detailTextLabel.text = UserPreferences.shared.llmToolsEnabled ? @"On" : @"Off";
        cell.accessoryType = UserPreferences.shared.llmToolsEnabled ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    }
    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == 0) {
        [self done:nil];
        return;
    }
    if (indexPath.row == 0) {
        [self showProviderPickerFromView:[tableView cellForRowAtIndexPath:indexPath]];
        return;
    }
    if (indexPath.row == 3)
        return;
    if (ISHLLMUsesAppleFoundationModels() && (indexPath.row == 1 || indexPath.row == 4))
        return;
    if (indexPath.row == 5) {
        [self queryAvailableModelsFromView:[tableView cellForRowAtIndexPath:indexPath]];
        return;
    }
    if (indexPath.row == 6) {
        [self testConnection];
        return;
    }
    if (indexPath.row == 7) {
        [self toggleShellToolsFromView:[tableView cellForRowAtIndexPath:indexPath]];
        return;
    }
    NSString *title = indexPath.row == 1 ? @"Server URL" : (indexPath.row == 2 ? @"Model" : @"API Key");
    NSString *current = indexPath.row == 1 ? UserPreferences.shared.llmServerURL : (indexPath.row == 2 ? UserPreferences.shared.llmModel : UserPreferences.shared.llmAPIKey);
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = current;
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.spellCheckingType = UITextSpellCheckingTypeNo;
        if (indexPath.row == 4)
            textField.secureTextEntry = YES;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        NSString *value = alert.textFields.firstObject.text ?: @"";
        if (indexPath.row == 1)
            UserPreferences.shared.llmServerURL = value;
        else if (indexPath.row == 2)
            UserPreferences.shared.llmModel = value;
        else
            UserPreferences.shared.llmAPIKey = value;
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
        message:@"The model will be able to request shell commands that run in the iSH Linux environment — for web search via curl/wget, reading files, or running programs. You confirm each command before it runs, output is capped at 64 KB, and commands are killed after 30 seconds. Only enable this with a model and server you trust."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Enable" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        UserPreferences.shared.llmToolsEnabled = YES;
        [self.tableView reloadData];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)showProviderPickerFromView:(UIView *)sourceView {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"LLM Provider"
                                                                   message:@"Choose a provider preset. Custom values can still be edited afterward."
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSDictionary<NSString *, NSString *> *preset in ISHLLMProviderPresets()) {
        NSString *name = preset[@"name"];
        NSString *title = [name isEqualToString:UserPreferences.shared.llmProvider]
            ? [name stringByAppendingString:@"  Current"]
            : name;
        [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
            UserPreferences.shared.llmProvider = name;
            UserPreferences.shared.llmServerURL = preset[@"url"] ?: @"";
            if (preset[@"model"].length > 0)
                UserPreferences.shared.llmModel = preset[@"model"];
            [self.tableView reloadData];
        }]];
    }
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self anchorPopoverForAlertController:alert toSource:sourceView];
    [self presentViewController:alert animated:YES completion:nil];
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

@implementation AboutViewController
{
    BOOL _didPresentInitialDiagnostics;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    [self _updateUI];
    UIBarButtonItem *workspaceButton = [[UIBarButtonItem alloc] initWithTitle:@"Workspace"
                                                                        style:UIBarButtonItemStylePlain
                                                                       target:self
                                                                       action:@selector(showWorkspace:)];
    if (self.recoveryMode) {
        self.includeDebugPanel = YES;
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
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
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

- (void)showWorkspace:(id)sender {
    (void) sender;
    // Open the Workspace full screen in the current scene, the same way launch-at-startup does
    // (SceneDelegate sets window.rootViewController to the workspace). The previous approach
    // asked iPadOS to activate a *separate* scene, which opened split screen on some iPads and
    // failed outright on others (Stage Manager / M4). Swapping the root sidesteps all of that,
    // and the scene's state restoration still reports a workspace scene because its root is one.
    UINavigationController *navigationController = ISHCreateWorkspaceNavigationControllerForTool(nil);
    UIWindow *window = self.view.window;
    if (window == nil) {
        [self presentViewController:navigationController animated:YES completion:nil];
        return;
    }
    [UIView transitionWithView:window
                      duration:0.3
                       options:UIViewAnimationOptionTransitionCrossDissolve
                    animations:^{
        BOOL wereAnimationsEnabled = UIView.areAnimationsEnabled;
        [UIView setAnimationsEnabled:NO];
        window.rootViewController = navigationController;
        [UIView setAnimationsEnabled:wereAnimationsEnabled];
    }
                    completion:nil];
}

- (void)_updateUI:(NSNotification *)notification {
    [self _updateUI];
}

- (void)_updateUI {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    self.disableDimmingSwitch.on = UserPreferences.shared.shouldDisableDimming;
    self.enableExperimentalAmd64JitSwitch.on = UserPreferences.shared.shouldEnableExperimentalAmd64Jit;
    self.enableMulticoreSwitch.on = UserPreferences.shared.shouldEnableMulticore;
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
    NSInteger sections = [super numberOfSectionsInTableView:self.tableView];
    if (!self.includeDebugPanel)
        sections--;
    return sections;
}

- (NSInteger)_llmSectionIndex {
    return [self _visibleStoryboardSectionCount];
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
        [UIApplication openURL:@"https://discord.com/channels/776432683302649866/776432683302649870"];
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
    if ([value isEqualToString:@"session-shell"])
        return value;
    return @"terminal";
}

- (NSString *)_initialWindowTitle {
    if ([[self _initialWindowPreferenceValue] isEqualToString:ISHInitialWindowWorkspaceValue])
        return @"Workspace";
    if ([[self _initialWindowPreferenceValue] isEqualToString:ISHInitialWindowChooseFilesystemValue])
        return @"Choose Filesystem";
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
                                            message:@"Choose whether new app launches open the Workspace, show a filesystem chooser, or open a terminal."
                                     preferredStyle:UIAlertControllerStyleActionSheet];

    NSString *currentValue = [self _initialWindowPreferenceValue];
    NSString *workspaceTitle = [currentValue isEqualToString:ISHInitialWindowWorkspaceValue]
        ? @"Workspace  Current"
        : @"Workspace";
    NSString *chooseFilesystemTitle = [currentValue isEqualToString:ISHInitialWindowChooseFilesystemValue]
        ? @"Choose Filesystem  Current"
        : @"Choose Filesystem";
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
    if (section == [self _llmSectionIndex])
        return @"LLM Client";
    return [super tableView:tableView titleForHeaderInSection:section];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return [self _visibleStoryboardSectionCount] + 1;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == [self _llmSectionIndex])
        return UserPreferences.shared.shouldEnableLLMClient ? 2 : 1;
    return [super tableView:tableView numberOfRowsInSection:section];
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == [self _llmSectionIndex]) {
        if (indexPath.row == 0)
            return [self _llmEnabledCell];
        return [self _llmSettingsCell];
    }
    return [super tableView:tableView cellForRowAtIndexPath:indexPath];
}

// iOS 27's UIKit bounds-checks the storyboard's static-section array. Our LLM
// section is appended at index == _llmSectionIndex (past the static sections),
// so any UITableViewController static *layout* delegate method reached via
// [super ...] for it indexes out of bounds -> "index N beyond bounds [0..N-1]"
// (reloadData asks the static layout for the appended section's heights). Older
// UIKit tolerated it; 27 throws. Guard these the same way we guard the
// data-source methods above. heightForRow/indentationLevel are part of the
// static-cell support (proven by the [super ...] calls above); header/footer
// heights may be table defaults, so only forward those when super implements them.
- (CGFloat)tableView:(UITableView *)tableView heightForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == [self _llmSectionIndex])
        return 44;
    return [super tableView:tableView heightForRowAtIndexPath:indexPath];
}

- (NSInteger)tableView:(UITableView *)tableView indentationLevelForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == [self _llmSectionIndex])
        return 0;
    return [super tableView:tableView indentationLevelForRowAtIndexPath:indexPath];
}

- (CGFloat)tableView:(UITableView *)tableView heightForHeaderInSection:(NSInteger)section {
    if (section == [self _llmSectionIndex])
        return UITableViewAutomaticDimension;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView heightForHeaderInSection:section];
    return UITableViewAutomaticDimension;
}

- (CGFloat)tableView:(UITableView *)tableView heightForFooterInSection:(NSInteger)section {
    if (section == [self _llmSectionIndex])
        return UITableViewAutomaticDimension;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView heightForFooterInSection:section];
    return UITableViewAutomaticDimension;
}

// Without an override here, UIKit falls back to its own default header/footer
// view synthesis (-[UITableViewDataSource tableView:viewForHeaderInSection:]),
// which hits the same static-section-array bounds check as the [super ...]
// calls above -- crashing on the appended LLM section ("index N beyond bounds
// [0..N-1]"). Returning nil lets UITableView fall back to the plain
// title-based header/footer from titleForHeaderInSection/titleForFooterInSection.
- (UIView *)tableView:(UITableView *)tableView viewForHeaderInSection:(NSInteger)section {
    if (section == [self _llmSectionIndex])
        return nil;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView viewForHeaderInSection:section];
    return nil;
}

- (UIView *)tableView:(UITableView *)tableView viewForFooterInSection:(NSInteger)section {
    if (section == [self _llmSectionIndex])
        return nil;
    if ([UITableViewController instancesRespondToSelector:_cmd])
        return [super tableView:tableView viewForFooterInSection:section];
    return nil;
}

- (IBAction)disableDimmingChanged:(id)sender {
    UserPreferences.shared.shouldDisableDimming = self.disableDimmingSwitch.on;
}

- (IBAction)enableExperimentalAmd64JitChanged:(id)sender {
    UserPreferences.shared.shouldEnableExperimentalAmd64Jit = self.enableExperimentalAmd64JitSwitch.on;
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
