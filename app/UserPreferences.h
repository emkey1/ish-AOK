//
//  UserPreferences.h
//  iSH
//
//  Created by Charlie Melbye on 11/12/18.
//

#import <UIKit/UIKit.h>
#import "Theme.h"

typedef NS_ENUM(NSInteger, CapsLockMapping) {
    __CapsLockMapFirst = 0,
    CapsLockMapNone = 0,
    CapsLockMapControl,
    CapsLockMapEscape,
    __CapsLockMapLast,
};

typedef enum : NSUInteger {
    __OptionMapFirst = 0,
    OptionMapNone = 0,
    OptionMapEsc,
    __OptionMapLast,
} OptionMapping;

typedef NS_ENUM(NSInteger, CursorStyle) {
    __CursorStyleFirst = 0,
    CursorStyleBlock = 0,
    CursorStyleBeam,
    CursorStyleUnderline,
    __CursorStyleLast,
};

typedef NS_ENUM(NSInteger, ColorScheme) {
    __ColorSchemeFirst = 0,
    ColorSchemeMatchSystem = 0,
    ColorSchemeAlwaysLight,
    ColorSchemeAlwaysDark,
    __ColorSchemeLast,
};

typedef NS_ENUM(NSInteger, WorkspaceStyle) {
    __WorkspaceStyleFirst = 0,
    WorkspaceStyleClassic = 0,
    WorkspaceStyleModern,
    __WorkspaceStyleLast,
};

NS_ASSUME_NONNULL_BEGIN

extern NSString *const kThemeForegroundColor;
extern NSString *const kThemeBackgroundColor;

@interface UserPreferences : NSObject

@property CapsLockMapping capsLockMapping;
@property OptionMapping optionMapping;
@property BOOL backtickMapEscape;
@property BOOL hideExtraKeysWithExternalKeyboard;
@property BOOL maximizeScreenSpace;
@property BOOL overrideControlSpace;
@property BOOL hideStatusBar;
@property BOOL showTerminalQuickButtons;
@property BOOL autoShowKeyboard;
@property NSInteger workspaceLaunchCount;
@property (nonatomic) Theme *theme;
@property (nonatomic) Palette *palette;
@property WorkspaceStyle workspaceStyle;
@property BOOL shouldDisableDimming;
@property BOOL shouldEnableMulticore;
@property BOOL shouldEnableHLE;
@property BOOL shouldEnableCryptoAccel;
@property BOOL shouldEnablePixAccel;
@property BOOL shouldEnableExtraLocking;
@property BOOL shouldEnableLLMClient;
@property (nonatomic) NSString *llmProvider;
@property (nonatomic) NSString *llmServerURL;
@property (nonatomic) NSString *llmModel;
@property (nonatomic) NSString *llmAPIKey;
// The saved set of chat destinations (endpoints) the user can switch between
// without opening settings, and which one is selected. Each entry is a
// dictionary of strings: "id", "name", "provider", "url", "model", "apiKey".
// The selected entry is mirrored into the four properties above, which stay
// the single source of truth for every request the client actually makes --
// these two only describe the saved set to pick from.
@property (nonatomic) NSArray<NSDictionary<NSString *, NSString *> *> *llmDestinations;
@property (nonatomic) NSString *llmActiveDestinationID;
@property BOOL llmToolsEnabled;
@property NSInteger llmToolTimeoutSeconds;
@property NSInteger llmToolOutputLimitKB;
@property NSInteger llmToolMaxRounds;
// When YES (the default), a reasoning model's <think>...</think> chain-of-thought is
// collapsed in the transcript behind a "Thinking" disclosure instead of being shown
// inline. The raw text is still stored and can be expanded and copied.
@property BOOL llmHideThinking;
@property (null_resettable) NSString *fontFamily;
@property (readonly) NSString *fontFamilyUserFacingName;
@property (readonly) UIFont *approximateFont;
@property NSNumber *fontSize;
@property (readonly) NSNumber *defaultFontSize;
@property ColorScheme colorScheme;
@property (readonly) BOOL requestingDarkAppearance;
@property (readonly) UIUserInterfaceStyle userInterfaceStyle API_AVAILABLE(ios(12.0));
@property (readonly) UIKeyboardAppearance keyboardAppearance;
@property CursorStyle cursorStyle;
@property (readonly) NSString *htermCursorShape;
@property BOOL blinkCursor;
@property (readonly) UIStatusBarStyle statusBarStyle;
@property NSArray<NSString *> *launchCommand;
@property NSArray<NSString *> *bootCommand;
@property (nonatomic) NSString *customDnsServers;
// When YES, new terminal sessions launched with the default "/bin/login -f root" command log in
// as the unprivileged UID 1000 account instead, matching how a real Linux desktop is normally
// used. Sessions with a customized launch command are unaffected.
@property BOOL shouldLoginAsDefaultUser;

+ (instancetype)shared;

- (BOOL)hasChangedLaunchCommand;

// Put these two back to what registerDefaults supplies. Setting the property to
// an empty array does NOT do this: the key would still exist, and a stored
// empty command is what leaves the app with no way to start a session at all.
- (void)resetLaunchCommand;
- (void)resetBootCommand;

@end

extern NSString *const kPreferenceLaunchCommandKey;
extern NSString *const kPreferenceBootCommandKey;
extern NSString *const kPreferenceInitialWindowKey;

// "Open Everything as Default User" targets whatever account this rootfs already has at UID
// 1000 (the conventional "first regular user" UID on Debian/Devuan/Alpine) -- no account is
// provisioned/created by iSH itself. See +[AppDelegate defaultUserAccountName], which looks up
// the actual account name (or returns nil if none exists) at each use.
extern const int ISHDefaultUserAccountUID;

NS_ASSUME_NONNULL_END
