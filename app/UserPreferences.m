//
//  UserPreferences.m
//  iSH
//
//  Created by Charlie Melbye on 11/12/18.
//

#import "UserPreferences.h"
#import "fs/proc/ish.h"
#include "jit/jit.h"
#include "task.h"

// Helpers for cleanup when extra locking is disabled.
extern bool doEnableExtraLocking;
extern lock_t pids_lock;
extern struct list alive_pids_list;

// IMPORTANT: If you add a constant here and expose it via UserPreferences,
// consider if it also needs to be exposed as a friendly preference and included
// in the KVO list below. (In most circumstances, the answer is "yes".)
static NSString *const kPreferenceCapsLockMappingKey = @"Caps Lock Mapping";
static NSString *const kPreferenceOptionMappingKey = @"Option Mapping";
static NSString *const kPreferenceBacktickEscapeKey = @"Backtick Mapping Escape";
static NSString *const kPreferenceHideExtraKeysWithExternalKeyboardKey = @"Hide Extra Keys With External Keyboard";
static NSString *const kPreferenceMaximizeScreenSpaceKey = @"Maximize Screen Space";
static NSString *const kPreferenceShowTerminalQuickButtonsKey = @"Show Terminal Quick Buttons";
static NSString *const kPreferenceAutoShowKeyboardKey = @"Auto Show Keyboard";
static NSString *const kPreferenceWorkspaceLaunchCountKey = @"Workspaces At Launch";
static NSString *const kPreferenceOverrideControlSpaceKey = @"Override Control Space";
static NSString *const kPreferenceFontFamilyKey = @"Font Family";
static NSString *const kPreferenceFontSizeKey = @"Font Size";
static NSString *const kPreferenceLineHeightKey = @"Line Height";
static NSString *const kPreferenceThemeKey = @"ModernTheme";
static NSString *const kPreferenceDisableDimmingKey = @"Disable Dimming";
static NSString *const kPreferenceEnableMulticoreKey = @"Enable Multicore";
static NSString *const kPreferenceEnableHLEKey = @"Enable HLE Accel";
static NSString *const kPreferenceEnableCryptoAccelKey = @"Enable Crypto Accel";
static NSString *const kPreferenceEnablePixAccelKey = @"Enable Pixman Accel";
static NSString *const kPreferenceEnableExtraLockingKey = @"Enable Additional Locking";
static NSString *const kPreferenceEnableLLMClientKey = @"Enable LLM Client";
static NSString *const kPreferenceLLMProviderKey = @"LLM Provider";
static NSString *const kPreferenceLLMServerURLKey = @"LLM Server URL";
static NSString *const kPreferenceLLMModelKey = @"LLM Model";
static NSString *const kPreferenceLLMAPIKeyKey = @"LLM API Key";
static NSString *const kPreferenceLLMDestinationsKey = @"LLM Destinations";
static NSString *const kPreferenceLLMActiveDestinationKey = @"LLM Active Destination";
static NSString *const kPreferenceLLMToolsEnabledKey = @"LLM Tools Enabled";
static NSString *const kPreferenceLLMToolTimeoutSecondsKey = @"LLM Tool Timeout Seconds";
static NSString *const kPreferenceLLMToolOutputLimitKBKey = @"LLM Tool Output Limit KB";
static NSString *const kPreferenceLLMToolMaxRoundsKey = @"LLM Tool Max Rounds";
static NSString *const kPreferenceLLMHideThinkingKey = @"LLM Hide Thinking";
static NSString *const kPreferenceCustomDnsServersKey = @"Custom DNS Servers";

NSString *const kPreferenceLaunchCommandKey = @"Init Command";
NSString *const kPreferenceBootCommandKey = @"Boot Command";
NSString *const kPreferenceInitialWindowKey = @"Initial Window";

// Defined next to the accessors that fall back to them; declared here so
// registerDefaults installs the same commands rather than a second copy.
static NSArray<NSString *> *ISHDefaultLaunchCommand(void);
static NSArray<NSString *> *ISHDefaultBootCommand(void);
static NSString *const kPreferenceLoginAsDefaultUserKey = @"Login As Default User";

const int ISHDefaultUserAccountUID = 1000;
static NSString *const kPreferenceCursorStyleKey = @"Cursor Style";
static NSString *const kPreferenceBlinkCursorKey = @"Blink Cursor";
NSString *const kPreferenceHideStatusBarKey = @"Status Bar";
static NSString *const kPreferenceColorSchemeKey = @"Color Scheme";
static NSString *const kPreferenceWorkspaceStyleKey = @"Workspace Style";

NSDictionary<NSString *, NSString *> *friendlyPreferenceMapping;
NSDictionary<NSString *, NSString *> *friendlyPreferenceReverseMapping;
NSDictionary<NSString *, NSString *> *kvoProperties;

extern bool doEnableMulticore;
static NSString *const kSystemMonospacedFontName = @"ui-monospace";

@interface UserPreferences ()
- (void)updateTheme;
@end

char **get_all_defaults_keys_impl(void) {
    NSArray<NSString *> *preferenceKeys = NSUserDefaults.standardUserDefaults.dictionaryRepresentation.allKeys;
    char **entries = malloc((preferenceKeys.count + 1) * sizeof(*entries));
    for (NSUInteger i = 0; i < preferenceKeys.count; ++i)
        entries[i] = strdup(preferenceKeys[i].UTF8String);
    entries[preferenceKeys.count] = NULL;
    return entries;
}

char *get_friendly_name_impl(const char *name) {
    const char *friendly_name = friendlyPreferenceReverseMapping[[NSString stringWithUTF8String:name]].UTF8String;
    if (friendly_name == NULL)
        return NULL;
    return strdup(friendly_name);
}

char *get_underlying_name_impl(const char *name) {
    return strdup(friendlyPreferenceMapping[[NSString stringWithUTF8String:name]].UTF8String);
}

bool get_user_default_impl(const char *name, char **buffer, size_t *size) {
    id value = [NSUserDefaults.standardUserDefaults objectForKey:[NSString stringWithUTF8String:name]];
    // Since we are writing with fragments, wrap the object in an array to have
    // a top-level object to check.
    if (!value || ![NSJSONSerialization isValidJSONObject:@[value]]) {
        return false;
    }
    NSError *error;
    NSJSONWritingOptions options = NSJSONWritingFragmentsAllowed | NSJSONWritingSortedKeys | NSJSONWritingPrettyPrinted;
    if (@available(iOS 13.0, *)) {
        options |= NSJSONWritingWithoutEscapingSlashes;
    }
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:options error:&error];
    if (error) {
        return false;
    }
    *buffer = malloc(data.length + 1);
    memcpy(*buffer, data.bytes, data.length);
    (*buffer)[data.length] = '\n';
    *size = data.length + 1;
    return true;
}

bool set_user_default_impl(const char *name, char *buffer, size_t size) {
    NSString *key = [NSString stringWithUTF8String:name];
    NSData *data = [NSData dataWithBytesNoCopy:buffer length:size freeWhenDone:NO];
    NSError *error;
    id value = [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingFragmentsAllowed error:&error];
    if (error) {
        return false;
    }
    NSString *property = kvoProperties[key];
    if (property) {
        if ([UserPreferences.shared validateValue:&value forKey:property error:nil]) {
            [UserPreferences.shared setValue:value forKey:property];
        } else {
            return false;
        }
    } else {
        [NSUserDefaults.standardUserDefaults setValue:value forKey:key];
    }
    return true;
}

bool remove_user_default_impl(const char *name) {
    NSString *key = [NSString stringWithUTF8String:name];
    NSString *property = kvoProperties[key];
    if (property) {
        [UserPreferences.shared willChangeValueForKey:property];
    }
    [NSUserDefaults.standardUserDefaults removeObjectForKey:key];
    if (property) {
        [UserPreferences.shared didChangeValueForKey:property];
    }
    
    // This particular property needs special handling to stay up-to-date
    if ([property isEqualToString:@"userTheme"]) {
        [UserPreferences.shared updateTheme];
    }
    return true;
}

// Backs /proc/ish/amd64_jit, the debugging lever for taking an amd64 guest off
// the JIT frontend and onto the interpreter (bisecting a suspected JIT bug).
// Deliberately not persisted: the JIT is the only supported configuration, so
// every launch starts on it and a knob left off can't silently follow the user
// forever. Same semantics as the CLI build (platform/standalone.c).
bool amd64_jit_preference_get(void) {
    return amd64_jit_is_enabled();
}

void amd64_jit_preference_set(bool enabled) {
    amd64_jit_set_enabled(enabled);
}

// TODO: Move these to Linux
#if ISH_LINUX
char **(*get_all_defaults_keys)(void);
char *(*get_friendly_name)(const char *name);
char *(*get_underlying_name)(const char *name);
bool (*get_user_default)(const char *name, char **buffer, size_t *size);
bool (*set_user_default)(const char *name, char *buffer, size_t size);
bool (*remove_user_default)(const char *name);
#endif

@implementation UserPreferences {
    NSUserDefaults *_defaults;
}

+ (instancetype)shared {
    static UserPreferences *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[self alloc] init];
    });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _defaults = [NSUserDefaults standardUserDefaults];
        [_defaults registerDefaults:@{
            kPreferenceEnableMulticoreKey: @(YES),
            kPreferenceEnableHLEKey: @(NO),
            kPreferenceEnableCryptoAccelKey: @(NO),
            kPreferenceEnablePixAccelKey: @(NO),
            kPreferenceEnableExtraLockingKey: @(YES),
            kPreferenceEnableLLMClientKey: @(NO),
            kPreferenceLLMProviderKey: @"OpenRouter Free",
            kPreferenceLLMServerURLKey: @"https://openrouter.ai/api/v1",
            kPreferenceLLMModelKey: @"openrouter/free",
            kPreferenceLLMAPIKeyKey: @"",
            kPreferenceLLMDestinationsKey: @[],
            kPreferenceLLMActiveDestinationKey: @"",
            kPreferenceLLMToolsEnabledKey: @(NO),
            kPreferenceLLMToolTimeoutSecondsKey: @(30),
            kPreferenceLLMToolOutputLimitKBKey: @(64),
            kPreferenceLLMToolMaxRoundsKey: @(20),
            kPreferenceLLMHideThinkingKey: @(YES),
            kPreferenceFontSizeKey: @(12),
            // 1 = the height hterm measures, i.e. exactly what every build
            // before this one did. Nobody's terminal moves until they ask.
            kPreferenceLineHeightKey: @(1),
            kPreferenceCapsLockMappingKey: @(CapsLockMapControl),
            kPreferenceOptionMappingKey: @(OptionMapNone),
            kPreferenceBacktickEscapeKey: @(NO),
            kPreferenceDisableDimmingKey: @(NO),
            kPreferenceLaunchCommandKey: ISHDefaultLaunchCommand(),
            kPreferenceBootCommandKey: ISHDefaultBootCommand(),
            kPreferenceInitialWindowKey: @"terminal",
            kPreferenceBlinkCursorKey: @(NO),
            kPreferenceCursorStyleKey: @(CursorStyleBlock),
            kPreferenceHideStatusBarKey: @(NO),
            kPreferenceColorSchemeKey: @(ColorSchemeAlwaysDark),
            kPreferenceWorkspaceStyleKey: @(WorkspaceStyleModern),
            kPreferenceShowTerminalQuickButtonsKey: @(YES),
            kPreferenceAutoShowKeyboardKey: @(YES),
            kPreferenceWorkspaceLaunchCountKey: @(1),
            kPreferenceThemeKey: @"Solarized",
            kPreferenceLoginAsDefaultUserKey: @(NO),
        }];
        // https://webkit.org/blog/10247/new-webkit-features-in-safari-13-1/
        if (@available(iOS 13.4, *)) {
            [_defaults registerDefaults:@{
                kPreferenceFontFamilyKey: kSystemMonospacedFontName,
            }];
        } else {
            [_defaults registerDefaults:@{
                kPreferenceFontFamilyKey: @"Menlo",
            }];
        }
        get_all_defaults_keys = get_all_defaults_keys_impl;
        get_friendly_name = get_friendly_name_impl;
        get_underlying_name = get_underlying_name_impl;
        get_user_default = get_user_default_impl;
        set_user_default = set_user_default_impl;
        remove_user_default = remove_user_default_impl;
        // The amd64 JIT is the only supported configuration on iOS; the
        // interpreter fallback survives purely as a debugging lever behind
        // /proc/ish/amd64_jit (and the per-program containment in jit.c that
        // keeps GNU `as` interpreted regardless).
        amd64_jit_set_enabled(true);
        friendlyPreferenceMapping = @{
            @"enable_multicore": kPreferenceEnableMulticoreKey,
            @"enable_hle": kPreferenceEnableHLEKey,
            @"enable_crypto_accel": kPreferenceEnableCryptoAccelKey,
            @"enable_pix_accel": kPreferenceEnablePixAccelKey,
            @"enable_extralocking": kPreferenceEnableExtraLockingKey,
            @"caps_lock_mapping": kPreferenceCapsLockMappingKey,
            @"option_mapping": kPreferenceOptionMappingKey,
            @"backtick_mapping_escape": kPreferenceBacktickEscapeKey,
            @"hide_extra_keys_with_external_keyboard": kPreferenceHideExtraKeysWithExternalKeyboardKey,
            @"maximize_screen_space": kPreferenceMaximizeScreenSpaceKey,
            @"show_terminal_quick_buttons": kPreferenceShowTerminalQuickButtonsKey,
            @"auto_show_keyboard": kPreferenceAutoShowKeyboardKey,
            @"workspace_launch_count": kPreferenceWorkspaceLaunchCountKey,
            @"override_control_space": kPreferenceOverrideControlSpaceKey,
            @"font_family": kPreferenceFontFamilyKey,
            @"font_size": kPreferenceFontSizeKey,
            @"line_height": kPreferenceLineHeightKey,
            @"disable_dimming": kPreferenceDisableDimmingKey,
            @"enable_llm_client": kPreferenceEnableLLMClientKey,
            @"llm_provider": kPreferenceLLMProviderKey,
            @"llm_server_url": kPreferenceLLMServerURLKey,
            @"llm_model": kPreferenceLLMModelKey,
            @"llm_api_key": kPreferenceLLMAPIKeyKey,
            @"llm_tools_enabled": kPreferenceLLMToolsEnabledKey,
            @"llm_tool_timeout_seconds": kPreferenceLLMToolTimeoutSecondsKey,
            @"llm_tool_output_limit_kb": kPreferenceLLMToolOutputLimitKBKey,
            @"llm_tool_max_rounds": kPreferenceLLMToolMaxRoundsKey,
            @"llm_hide_thinking": kPreferenceLLMHideThinkingKey,
            @"launch_command": kPreferenceLaunchCommandKey,
            @"boot_command": kPreferenceBootCommandKey,
            @"login_as_default_user": kPreferenceLoginAsDefaultUserKey,
            @"cursor_style": kPreferenceCursorStyleKey,
            @"blink_cursor": kPreferenceBlinkCursorKey,
            @"hide_status_bar": kPreferenceHideStatusBarKey,
            @"color_scheme": kPreferenceColorSchemeKey,
            @"workspace_style": kPreferenceWorkspaceStyleKey,
            @"theme": kPreferenceThemeKey,
        };
        NSMutableDictionary <NSString *, NSString *> *reverseMapping = [NSMutableDictionary new];
        for (NSString *key in friendlyPreferenceMapping) {
            reverseMapping[friendlyPreferenceMapping[key]] = key;
        }
        friendlyPreferenceReverseMapping = reverseMapping;
        // Helps a bit with compile-time safety and autocompletion
#define property(x) NSStringFromSelector(@selector(x))
        kvoProperties = @{
            kPreferenceEnableMulticoreKey: property(shouldEnableMulticore),
            kPreferenceEnableHLEKey: property(shouldEnableHLE),
            kPreferenceEnableCryptoAccelKey: property(shouldEnableCryptoAccel),
            kPreferenceEnablePixAccelKey: property(shouldEnablePixAccel),
	        kPreferenceEnableExtraLockingKey: property(shouldEnableExtraLocking),
            kPreferenceCapsLockMappingKey: property(capsLockMapping),
            kPreferenceOptionMappingKey: property(optionMapping),
            kPreferenceBacktickEscapeKey: property(backtickMapEscape),
            kPreferenceHideExtraKeysWithExternalKeyboardKey: property(hideExtraKeysWithExternalKeyboard),
            kPreferenceMaximizeScreenSpaceKey: property(maximizeScreenSpace),
            kPreferenceShowTerminalQuickButtonsKey: property(showTerminalQuickButtons),
            kPreferenceAutoShowKeyboardKey: property(autoShowKeyboard),
            kPreferenceWorkspaceLaunchCountKey: property(workspaceLaunchCount),
            kPreferenceOverrideControlSpaceKey: property(overrideControlSpace),
            kPreferenceFontFamilyKey: property(fontFamily),
            kPreferenceFontSizeKey: property(fontSize),
            kPreferenceLineHeightKey: property(lineHeight),
            kPreferenceDisableDimmingKey: property(shouldDisableDimming),
            kPreferenceEnableLLMClientKey: property(shouldEnableLLMClient),
            kPreferenceLLMProviderKey: property(llmProvider),
            kPreferenceLLMServerURLKey: property(llmServerURL),
            kPreferenceLLMModelKey: property(llmModel),
            kPreferenceLLMAPIKeyKey: property(llmAPIKey),
            kPreferenceLLMDestinationsKey: property(llmDestinations),
            kPreferenceLLMActiveDestinationKey: property(llmActiveDestinationID),
            kPreferenceLLMToolsEnabledKey: property(llmToolsEnabled),
            kPreferenceLLMToolTimeoutSecondsKey: property(llmToolTimeoutSeconds),
            kPreferenceLLMToolOutputLimitKBKey: property(llmToolOutputLimitKB),
            kPreferenceLLMToolMaxRoundsKey: property(llmToolMaxRounds),
            kPreferenceLLMHideThinkingKey: property(llmHideThinking),
            kPreferenceCustomDnsServersKey: property(customDnsServers),
            kPreferenceLaunchCommandKey: property(launchCommand),
            kPreferenceBootCommandKey: property(bootCommand),
            kPreferenceCursorStyleKey: property(cursorStyle),
            kPreferenceBlinkCursorKey: property(blinkCursor),
            kPreferenceHideStatusBarKey: property(hideStatusBar),
            kPreferenceColorSchemeKey: property(colorScheme),
            kPreferenceWorkspaceStyleKey: property(workspaceStyle),
            // This one is a little bit special, so it needs extra handling.
            // The backing property for this is intentionally underscored.
            kPreferenceThemeKey: @"userTheme",
        };
#undef property
        
        [self updateTheme];
        
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateTheme:) name:ThemesUpdatedNotification object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateTheme:) name:ThemeUpdatedNotification object:nil];
    }
    return self;
}

// MARK: - Preference properties

// MARK: capsLockMapping
- (CapsLockMapping)capsLockMapping {
    return [_defaults integerForKey:kPreferenceCapsLockMappingKey];
}

- (void)setCapsLockMapping:(CapsLockMapping)capsLockMapping {
    [_defaults setInteger:capsLockMapping forKey:kPreferenceCapsLockMappingKey];
}

- (BOOL)validateCapsLockMapping:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSNumber.class]) {
        return NO;
    }
    int _value = [(NSNumber *)(*value) intValue];
    return _value >= __CapsLockMapFirst && _value < __CapsLockMapLast;
}

// MARK: optionMapping
- (OptionMapping)optionMapping {
    return [_defaults integerForKey:kPreferenceOptionMappingKey];
}

- (void)setOptionMapping:(OptionMapping)optionMapping {
    [_defaults setInteger:optionMapping forKey:kPreferenceOptionMappingKey];
}

- (BOOL)validateOptionMapping:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSNumber.class]) {
        return NO;
    }
    int _value = [(NSNumber *)(*value) intValue];
    return _value >= __OptionMapFirst && _value < __OptionMapLast;
}

// MARK: backtickMapEscape
- (BOOL)backtickMapEscape {
    return [_defaults boolForKey:kPreferenceBacktickEscapeKey];
}

- (void)setBacktickMapEscape:(BOOL)backtickMapEscape {
    [_defaults setBool:backtickMapEscape forKey:kPreferenceBacktickEscapeKey];
}

- (BOOL)validateBacktickMapEscape:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: hideExtraKeysWithExternalKeyboard
- (BOOL)hideExtraKeysWithExternalKeyboard {
    return [_defaults boolForKey:kPreferenceHideExtraKeysWithExternalKeyboardKey];
}

- (void)setHideExtraKeysWithExternalKeyboard:(BOOL)hideExtraKeysWithExternalKeyboard {
    [_defaults setBool:hideExtraKeysWithExternalKeyboard forKey:kPreferenceHideExtraKeysWithExternalKeyboardKey];
}

- (BOOL)validateHideExtraKeysWithExternalKeyboard:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: maximizeScreenSpace
- (BOOL)maximizeScreenSpace {
    return [_defaults boolForKey:kPreferenceMaximizeScreenSpaceKey];
}

- (void)setMaximizeScreenSpace:(BOOL)maximizeScreenSpace {
    [_defaults setBool:maximizeScreenSpace forKey:kPreferenceMaximizeScreenSpaceKey];
}

// MARK: showTerminalQuickButtons
- (BOOL)showTerminalQuickButtons {
    return [_defaults boolForKey:kPreferenceShowTerminalQuickButtonsKey];
}

- (void)setShowTerminalQuickButtons:(BOOL)showTerminalQuickButtons {
    [_defaults setBool:showTerminalQuickButtons forKey:kPreferenceShowTerminalQuickButtonsKey];
}

// MARK: autoShowKeyboard
- (BOOL)autoShowKeyboard {
    return [_defaults boolForKey:kPreferenceAutoShowKeyboardKey];
}

- (void)setAutoShowKeyboard:(BOOL)autoShowKeyboard {
    [_defaults setBool:autoShowKeyboard forKey:kPreferenceAutoShowKeyboardKey];
}

// MARK: workspaceLaunchCount
- (NSInteger)workspaceLaunchCount {
    NSInteger value = [_defaults integerForKey:kPreferenceWorkspaceLaunchCountKey];
    return MIN(MAX(value, (NSInteger)1), (NSInteger)4);
}

- (void)setWorkspaceLaunchCount:(NSInteger)workspaceLaunchCount {
    [_defaults setInteger:MIN(MAX(workspaceLaunchCount, (NSInteger)1), (NSInteger)4)
                   forKey:kPreferenceWorkspaceLaunchCountKey];
}

// MARK: overrideControlSpace
- (BOOL)overrideControlSpace {
    return [_defaults boolForKey:kPreferenceOverrideControlSpaceKey];
}

- (void)setOverrideControlSpace:(BOOL)overrideControlSpace {
    [_defaults setBool:overrideControlSpace forKey:kPreferenceOverrideControlSpaceKey];
}

- (BOOL)validateOverrideControlSpace:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: fontSize
- (NSNumber *)fontSize {
    return [_defaults objectForKey:kPreferenceFontSizeKey];
}

- (void)setFontSize:(NSNumber *)fontSize {
    [_defaults setObject:fontSize forKey:kPreferenceFontSizeKey];
}

- (BOOL)validateFontSize:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: lineHeight
- (NSNumber *)lineHeight {
    NSNumber *value = [_defaults objectForKey:kPreferenceLineHeightKey];
    return value != nil ? value : @(1);
}

- (void)setLineHeight:(NSNumber *)lineHeight {
    [_defaults setObject:lineHeight forKey:kPreferenceLineHeightKey];
}

// The same bounds hterm's setLineHeight enforces. Below about half the measured
// height even capitals are cut, so this is a floor against nonsense rather than
// a claim that everything inside it looks right -- how much of the em a patched
// font's block glyphs cover varies between Nerd Font patches, which is why this
// is a multiplier to tune and not a formula.
- (BOOL)validateLineHeight:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSNumber.class])
        return NO;
    double scale = [*value doubleValue];
    return scale > 0.5 && scale <= 2;
}

- (NSNumber *)defaultFontSize {
    NSNumber *defaultFontSize = [[[NSUserDefaults alloc] initWithSuiteName:NSRegistrationDomain] objectForKey:kPreferenceFontSizeKey];
    return defaultFontSize ?: @12;
}

- (BOOL)validatesetLockSleepNanoseconds:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: fontFamily
- (NSString *)fontFamily {
    return [_defaults objectForKey:kPreferenceFontFamilyKey];
}

- (void)setFontFamily:(NSString *)fontFamily {
    if (fontFamily) {
        [_defaults setObject:fontFamily forKey:kPreferenceFontFamilyKey];
    } else {
        [_defaults removeObjectForKey:kPreferenceFontFamilyKey];
    }
}

- (BOOL)validateFontFamily:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSString.class];
}

- (NSString *)fontFamilyUserFacingName {
    return [self.fontFamily isEqualToString:kSystemMonospacedFontName] ? @"System" : self.fontFamily;
}

- (UIFont *)approximateFont {
    if (@available(iOS 13.4, *)) {
        if ([self.fontFamily isEqualToString:kSystemMonospacedFontName]) {
            return [UIFont monospacedSystemFontOfSize:self.fontSize.doubleValue weight:UIFontWeightRegular];
        }
    }
    UIFont *font = [UIFont fontWithName:self.fontFamily size:self.fontSize.doubleValue];
    return font ? font : [UIFont fontWithName:@"Menlo" size:self.fontSize.doubleValue];
}

// MARK: theme
- (void)setTheme:(Theme *)theme {
    _theme = theme;
    [_defaults setObject:theme.name forKey:kPreferenceThemeKey];
}

// These are provided because user theme validation is done with strings
- (NSString *)_userTheme {
    return self.theme.name;
}

- (void)_setUserTheme:(NSString *)userTheme {
    Theme *theme;
    if ((theme = [Theme themeForName:userTheme includingDefaultThemes:YES])) {
        self.theme = theme;
    } else {
        self.theme = Theme.defaultThemes.lastObject;
    }
}

- (BOOL)validateUserTheme:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSString.class];
}

- (void)updateTheme:(NSNotification *)notification {
    if (notification.object) {
        [_defaults setValue:notification.object forKey:kPreferenceThemeKey];
    }
    [self updateTheme];
}

- (void)updateTheme {
    [self _setUserTheme:[_defaults valueForKey:kPreferenceThemeKey]];
}

- (Palette *)palette {
    switch (self.colorScheme) {
        case ColorSchemeMatchSystem:
            return self.class.systemThemeIsDark ? self.theme.darkPalette : self.theme.lightPalette;
        case ColorSchemeAlwaysDark:
            return self.theme.darkPalette;
        default:
            NSAssert(NO, @"invalid color scheme");
        case ColorSchemeAlwaysLight:
            return self.theme.lightPalette;
    }
}

// MARK: shouldDisableDimming
- (BOOL)shouldDisableDimming {
    return [_defaults boolForKey:kPreferenceDisableDimmingKey];
}

- (void)setShouldDisableDimming:(BOOL)dim {
    [_defaults setBool:dim forKey:kPreferenceDisableDimmingKey];
}

- (BOOL)validateShouldDisableDimming:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: shouldEnableLLMClient
- (BOOL)shouldEnableLLMClient {
    return [_defaults boolForKey:kPreferenceEnableLLMClientKey];
}

- (void)setShouldEnableLLMClient:(BOOL)enabled {
    [_defaults setBool:enabled forKey:kPreferenceEnableLLMClientKey];
}

- (BOOL)validateShouldEnableLLMClient:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: llmProvider
- (NSString *)llmProvider {
    return [_defaults stringForKey:kPreferenceLLMProviderKey] ?: @"Custom";
}

- (void)setLlmProvider:(NSString *)llmProvider {
    [_defaults setObject:llmProvider ?: @"Custom" forKey:kPreferenceLLMProviderKey];
}

- (BOOL)validateLlmProvider:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSString.class];
}

// MARK: llmServerURL
- (NSString *)llmServerURL {
    return [_defaults stringForKey:kPreferenceLLMServerURLKey] ?: @"";
}

- (void)setLlmServerURL:(NSString *)llmServerURL {
    [_defaults setObject:llmServerURL ?: @"" forKey:kPreferenceLLMServerURLKey];
}

- (BOOL)validateLlmServerURL:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSString.class];
}

// MARK: llmModel
- (NSString *)llmModel {
    return [_defaults stringForKey:kPreferenceLLMModelKey] ?: @"";
}

- (void)setLlmModel:(NSString *)llmModel {
    [_defaults setObject:llmModel ?: @"" forKey:kPreferenceLLMModelKey];
}

- (BOOL)validateLlmModel:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSString.class];
}

// MARK: llmAPIKey
- (NSString *)llmAPIKey {
    return [_defaults stringForKey:kPreferenceLLMAPIKeyKey] ?: @"";
}

- (void)setLlmAPIKey:(NSString *)llmAPIKey {
    [_defaults setObject:llmAPIKey ?: @"" forKey:kPreferenceLLMAPIKeyKey];
}

// MARK: llmDestinations
// Deliberately not exposed through friendlyPreferenceMapping: an array of
// dictionaries has no sensible string form for the guest-side `defaults`
// tool, and a destination id set there without the matching four scalars
// would make the next settings edit write into the wrong saved entry.
- (NSArray<NSDictionary<NSString *, NSString *> *> *)llmDestinations {
    NSArray *stored = [_defaults arrayForKey:kPreferenceLLMDestinationsKey];
    return [stored isKindOfClass:NSArray.class] ? stored : @[];
}

- (void)setLlmDestinations:(NSArray<NSDictionary<NSString *, NSString *> *> *)llmDestinations {
    [_defaults setObject:llmDestinations ?: @[] forKey:kPreferenceLLMDestinationsKey];
}

// MARK: llmActiveDestinationID
- (NSString *)llmActiveDestinationID {
    return [_defaults stringForKey:kPreferenceLLMActiveDestinationKey] ?: @"";
}

- (void)setLlmActiveDestinationID:(NSString *)llmActiveDestinationID {
    [_defaults setObject:llmActiveDestinationID ?: @"" forKey:kPreferenceLLMActiveDestinationKey];
}

- (BOOL)validateLlmAPIKey:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSString.class];
}

// MARK: llmToolsEnabled
- (BOOL)llmToolsEnabled {
    return [_defaults boolForKey:kPreferenceLLMToolsEnabledKey];
}

- (void)setLlmToolsEnabled:(BOOL)llmToolsEnabled {
    [_defaults setBool:llmToolsEnabled forKey:kPreferenceLLMToolsEnabledKey];
}

- (BOOL)validateLlmToolsEnabled:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: llmToolTimeoutSeconds
- (NSInteger)llmToolTimeoutSeconds {
    return [_defaults integerForKey:kPreferenceLLMToolTimeoutSecondsKey];
}

- (void)setLlmToolTimeoutSeconds:(NSInteger)llmToolTimeoutSeconds {
    [_defaults setInteger:llmToolTimeoutSeconds forKey:kPreferenceLLMToolTimeoutSecondsKey];
}

- (BOOL)validateLlmToolTimeoutSeconds:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: llmToolOutputLimitKB
- (NSInteger)llmToolOutputLimitKB {
    return [_defaults integerForKey:kPreferenceLLMToolOutputLimitKBKey];
}

- (void)setLlmToolOutputLimitKB:(NSInteger)llmToolOutputLimitKB {
    [_defaults setInteger:llmToolOutputLimitKB forKey:kPreferenceLLMToolOutputLimitKBKey];
}

- (BOOL)validateLlmToolOutputLimitKB:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: llmToolMaxRounds
- (NSInteger)llmToolMaxRounds {
    return [_defaults integerForKey:kPreferenceLLMToolMaxRoundsKey];
}

- (void)setLlmToolMaxRounds:(NSInteger)llmToolMaxRounds {
    [_defaults setInteger:llmToolMaxRounds forKey:kPreferenceLLMToolMaxRoundsKey];
}

- (BOOL)validateLlmToolMaxRounds:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: llmHideThinking
- (BOOL)llmHideThinking {
    return [_defaults boolForKey:kPreferenceLLMHideThinkingKey];
}

- (void)setLlmHideThinking:(BOOL)llmHideThinking {
    [_defaults setBool:llmHideThinking forKey:kPreferenceLLMHideThinkingKey];
}

- (BOOL)validateLlmHideThinking:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: customDnsServers
- (NSString *)customDnsServers {
    return [_defaults stringForKey:kPreferenceCustomDnsServersKey] ?: @"";
}

- (void)setCustomDnsServers:(NSString *)customDnsServers {
    [_defaults setObject:customDnsServers ?: @"" forKey:kPreferenceCustomDnsServersKey];
}

- (BOOL)validateCustomDnsServers:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSString.class];
}

// MARK: ShouldEnablemulticore
- (BOOL)shouldEnableMulticore {
    return [_defaults boolForKey:kPreferenceEnableMulticoreKey];
}

- (void)setShouldEnableMulticore:(BOOL)dim {
    [_defaults setBool:dim forKey:kPreferenceEnableMulticoreKey];
}

- (BOOL)validateShouldEnableMulticore:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: ShouldEnableHLE
- (BOOL)shouldEnableHLE {
    return [_defaults boolForKey:kPreferenceEnableHLEKey];
}

- (void)setShouldEnableHLE:(BOOL)dim {
    [_defaults setBool:dim forKey:kPreferenceEnableHLEKey];
}

- (BOOL)validateShouldEnableHLE:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: ShouldEnableCryptoAccel
- (BOOL)shouldEnableCryptoAccel {
    return [_defaults boolForKey:kPreferenceEnableCryptoAccelKey];
}

- (void)setShouldEnableCryptoAccel:(BOOL)dim {
    [_defaults setBool:dim forKey:kPreferenceEnableCryptoAccelKey];
}

- (BOOL)validateShouldEnableCryptoAccel:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: ShouldEnablePixAccel
- (BOOL)shouldEnablePixAccel {
    return [_defaults boolForKey:kPreferenceEnablePixAccelKey];
}

- (void)setShouldEnablePixAccel:(BOOL)dim {
    [_defaults setBool:dim forKey:kPreferenceEnablePixAccelKey];
}

- (BOOL)validateShouldEnablePixAccel:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: ShouldEnableExtraLocking
- (BOOL)shouldEnableExtraLocking {
    return [_defaults boolForKey:kPreferenceEnableExtraLockingKey];
}

- (void)setShouldEnableExtraLocking:(BOOL)dim {
    [_defaults setBool:dim forKey:kPreferenceEnableExtraLockingKey];
}

- (BOOL)validateShouldEnableExtraLocking:(id *)value error:(NSError **)error {
    // Toggling this at runtime still needs coordinated cleanup of active tasks.
    if(doEnableExtraLocking == true) {
//        complex_lockt(&pids_lock, 0, __FILE__, __LINE__);
 //       zero_critical_regions_count();
  //      unlock(&pids_lock);
    }
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: shouldLoginAsDefaultUser
- (BOOL)shouldLoginAsDefaultUser {
    return [_defaults boolForKey:kPreferenceLoginAsDefaultUserKey];
}

- (void)setShouldLoginAsDefaultUser:(BOOL)shouldLoginAsDefaultUser {
    [_defaults setBool:shouldLoginAsDefaultUser forKey:kPreferenceLoginAsDefaultUserKey];
}

- (BOOL)validateShouldLoginAsDefaultUser:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: launchCommand

// Sanitize on the way OUT, not only on the way in. A launch command that cannot
// start anything is exactly the state in which no session opens -- so there is
// no terminal to fix it from, and the bad value survives every restart because
// registerDefaults only supplies a value for an ABSENT key. A build carrying
// this heals itself; nothing has to be typed into Settings to recover.
//
// Two ways a stored command goes bad, both seen on a device:
//   - empty components, from a trailing or doubled space in the text field
//     (@[@"/bin/login", @"-f", @"root", @""] -- login gets "" as the username);
//   - nothing but empty components, from clearing the field, which used to
//     store @[@""] rather than removing the key.
static NSArray<NSString *> *ISHUsableCommandOrDefault(NSArray<NSString *> *command,
                                                      NSArray<NSString *> *fallback) {
    NSMutableArray<NSString *> *words = [NSMutableArray arrayWithCapacity:command.count];
    for (id word in command) {
        if ([word isKindOfClass:NSString.class] && ((NSString *) word).length != 0)
            [words addObject:word];
    }
    return words.count != 0 ? words : fallback;
}

// Also what registerDefaults installs, so the two cannot drift -- the fallback
// below has to be the same command a fresh install would get.
static NSArray<NSString *> *ISHDefaultLaunchCommand(void) {
    return @[@"/bin/login", @"-f", @"root"];
}

static NSArray<NSString *> *ISHDefaultBootCommand(void) {
    return @[@"/sbin/init"];
}

- (NSArray<NSString *> *)launchCommand {
    return ISHUsableCommandOrDefault([_defaults stringArrayForKey:kPreferenceLaunchCommandKey],
                                     ISHDefaultLaunchCommand());
}

- (void)setLaunchCommand:(NSArray<NSString *> *)launchCommand {
    [_defaults setObject:launchCommand forKey:kPreferenceLaunchCommandKey];
}

// removeObjectForKey, not setObject:@[]. registerDefaults only supplies a value
// for a key that is ABSENT, so storing an empty array is not a return to the
// default -- it is a command that can never start anything, and it survives
// restarts.
- (void)resetLaunchCommand {
    [_defaults removeObjectForKey:kPreferenceLaunchCommandKey];
}

- (BOOL)validateLaunchCommand:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSArray.class]) {
        return NO;
    }
    for (id element in (NSArray *)(*value)) {
        if (![element isKindOfClass:NSString.class]) {
            return NO;
        }
    }
    return YES;
}

- (BOOL)hasChangedLaunchCommand {
    NSArray *defaultLaunchCommand = [[[NSUserDefaults alloc] initWithSuiteName:NSRegistrationDomain] stringArrayForKey:kPreferenceLaunchCommandKey];
    return ![self.launchCommand isEqual:defaultLaunchCommand];
}

// MARK: bootCommand
- (NSArray<NSString *> *)bootCommand {
    return ISHUsableCommandOrDefault([_defaults stringArrayForKey:kPreferenceBootCommandKey],
                                     ISHDefaultBootCommand());
}

- (void)setBootCommand:(NSArray<NSString *> *)bootCommand {
    [_defaults setObject:bootCommand forKey:kPreferenceBootCommandKey];
}

- (void)resetBootCommand {
    [_defaults removeObjectForKey:kPreferenceBootCommandKey];
}

- (BOOL)validateBootCommand:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSArray.class]) {
        return NO;
    }
    for (id element in (NSArray *)(*value)) {
        if (![element isKindOfClass:NSString.class]) {
            return NO;
        }
    }
    return YES;
}

// MARK: cursorStyle

- (CursorStyle)cursorStyle {
    return [_defaults integerForKey:kPreferenceCursorStyleKey];
}

- (void)setCursorStyle:(CursorStyle)cursorStyle {
    [_defaults setInteger:cursorStyle forKey:kPreferenceCursorStyleKey];
}

- (BOOL)validateCursorStyle:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSNumber.class]) {
        return NO;
    }
    int _value = [(NSNumber *)(*value) intValue];
    return _value >= __CursorStyleLast && value < __CursorStyleFirst;
}

- (NSString *)htermCursorShape {
    switch (self.cursorStyle) {
        case CursorStyleBlock:
            return @"BLOCK";
        case CursorStyleBeam:
            return @"BEAM";
        case CursorStyleUnderline:
            return @"UNDERLINE";
        default:
            NSAssert(NO, @"Invalid cursor style");
            return nil;
    }
}

// MARK: blinkCursor

- (BOOL)blinkCursor {
    return [_defaults boolForKey:kPreferenceBlinkCursorKey];
}

- (void)setBlinkCursor:(BOOL)blinkCursor {
    [_defaults setBool:blinkCursor forKey:kPreferenceBlinkCursorKey];
}

- (BOOL)validateBlinkCursor:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: hideStatusBar
- (BOOL)hideStatusBar {
    return [_defaults boolForKey:kPreferenceHideStatusBarKey];
}

- (void)setHideStatusBar:(BOOL)showStatusBar {
    [_defaults setBool:showStatusBar forKey:kPreferenceHideStatusBarKey];
}

- (BOOL)validateHideStatusBar:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

- (ColorScheme)colorScheme {
    return [_defaults integerForKey:kPreferenceColorSchemeKey];
}

- (void)setColorScheme:(ColorScheme)colorScheme {
    [_defaults setInteger:colorScheme forKey:kPreferenceColorSchemeKey];
}

- (BOOL)validateColorScheme:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSNumber.class]) {
        return NO;
    }
    int _value = [(NSNumber *)(*value) intValue];
    return _value >= __ColorSchemeLast && value < __ColorSchemeFirst;
}

// MARK: workspaceStyle
- (WorkspaceStyle)workspaceStyle {
    return [_defaults integerForKey:kPreferenceWorkspaceStyleKey];
}

- (void)setWorkspaceStyle:(WorkspaceStyle)workspaceStyle {
    [_defaults setInteger:workspaceStyle forKey:kPreferenceWorkspaceStyleKey];
}

- (BOOL)validateWorkspaceStyle:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSNumber.class]) {
        return NO;
    }
    int _value = [(NSNumber *)(*value) intValue];
    return _value >= __WorkspaceStyleFirst && _value < __WorkspaceStyleLast;
}

+ (BOOL)systemThemeIsDark {
    if (@available(iOS 12.0, *)) {
        switch (UIScreen.mainScreen.traitCollection.userInterfaceStyle) {
            case UIUserInterfaceStyleLight:
                return NO;
            case UIUserInterfaceStyleDark:
                return YES;
            default:
                break;
        }
    }
    return NO;
}

- (BOOL)requestingDarkAppearance {
    return (self.class.systemThemeIsDark && !self.theme.appearance.darkOverride) || (!self.class.systemThemeIsDark && self.theme.appearance.lightOverride);
}

- (UIUserInterfaceStyle)userInterfaceStyle {
    return self.requestingDarkAppearance ? UIUserInterfaceStyleDark : UIUserInterfaceStyleLight;
}

- (UIKeyboardAppearance)keyboardAppearance {
    return self.requestingDarkAppearance ? UIKeyboardAppearanceDark : UIKeyboardAppearanceLight;
}

- (UIStatusBarStyle)statusBarStyle {
    return self.requestingDarkAppearance ? UIStatusBarStyleLightContent : UIStatusBarStyleDefault;
}

@end
