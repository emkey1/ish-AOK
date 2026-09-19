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
// Simulated swap (docs/simulated_swap_plan.md section 3.13). Paging guest
// memory writes to the device's flash and occupies container space, so it is
// not a thing to turn on for somebody: swap ships OFF, and the size is asked
// for rather than derived from device RAM or free disk. Both halves matter --
// swap runs for a launch only when shouldEnableSwap is YES *and* swapSizeMB is
// greater than zero, so "enabled with no size chosen" is still off, and the
// guest sees byte-for-byte what it sees today (SwapTotal 0, a header-only
// /proc/swaps, swapon refusing). Both go across untouched:
// ISHPublishSwapConfiguration in app/AppDelegate.m hands the pair to
// swap_set_preference(), and swap_startup() in kernel/swap.c is the one place
// that refuses an enable with no size -- and says so -- rather than this side
// folding them together and losing the difference between "not asked for" and
// "asked for and could not start".
//
// Read once per launch. Changing either takes effect at the next launch; a
// running pager is deliberately not resizable in place.
@property BOOL shouldEnableSwap;

// Suspend to disk (kernel/checkpoint.c). When the app is backgrounded it
// writes the whole guest -- every process, its memory, its descriptors, and
// whatever a native shell says about itself -- to one file, and the next
// launch resumes from it instead of booting.
//
// OFF by default, on the same reasoning swap is: it spends the user's storage
// and, if a session cannot be described, it can lose one. A checkpoint that
// refuses says so in /proc/ish/checkpoint and the app boots normally, so the
// worst case is the behaviour you get with this off.
@property BOOL shouldSuspendToDisk;
// 0 means "the user has not chosen a size", which is the registered default
// and keeps swap off. Clamped to 0...ISHSwapMaxSizeMB on the way in AND on the
// way out: the iOS Settings pane (app/Settings.bundle/Root.plist) writes this
// key into NSUserDefaults with none of our code running, so the setter is not
// the only writer and cannot be the only place the range is enforced.
@property NSInteger swapSizeMB;

// Swap to an external USB drive instead of the app's container. When ON,
// swap reads/writes go to a .aok-swap file in a user-chosen directory on
// a plugged-in USB drive, via a security-scoped bookmark. The bookmark is
// presented through a UIDocumentPickerViewController the first time the
// user enables this; subsequent launches restore it automatically.
//
// Both take effect at the next launch, like shouldEnableSwap and swapSizeMB.
// If the bookmark is stale or the drive is not attached, swap stays off and
// the status text says why.
@property BOOL shouldEnableSwapOnExternal;
@property (nonatomic) NSData *swapExternalBookmark;
@property (nonatomic) NSString *swapExternalPath;

// Compressed memory (kernel/zswap.h). A cache in FRONT of the swap area: a
// frame that compresses is kept in RAM instead of written to flash, which costs
// no write budget and comes back about two orders of magnitude faster than a
// disk read. Measured 2.2-2.8x on real workloads, at 1.9 us per decompress on
// an M4 and 3.1 us on an A9.
//
// Needs swap on -- it fronts the area rather than replacing it -- and is off
// with its own switch even then, because it spends the user's RAM and how much
// is their decision. Same 0-means-off rule as swapSizeMB.
@property BOOL shouldEnableCompressedMemory;
@property NSInteger compressedMemorySizeMB;
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
// Saved command-line snippets for the terminal's snippet sheet: an array of
// {id, name, text, run} dictionaries, keyed by the constants in Snippets.h.
@property (nonatomic) NSArray<NSDictionary<NSString *, id> *> *snippets;
// SHA-256, hex, of the snippet JSON as it was last written to or read from the
// guest-visible mirror. This is what makes "the guest file changed underneath
// us" an answerable question rather than a guess -- see +[ISHSnippetStore
// syncWithGuest:].
@property (nonatomic) NSString *snippetsSyncedDigest;
@property (nonatomic) NSString *llmActiveDestinationID;
@property BOOL llmToolsEnabled;
@property NSInteger llmToolTimeoutSeconds;
@property NSInteger llmToolOutputLimitKB;
@property NSInteger llmToolMaxRounds;
// When YES (the default), a reasoning model's <think>...</think> chain-of-thought is
// collapsed in the transcript behind a "Thinking" disclosure instead of being shown
// inline. The raw text is still stored and can be expanded and copied.
@property BOOL llmHideThinking;
// Gate for the Shortcuts "Run Command" App Intent (default YES). Off, the
// action fails with an error naming this setting instead of running anything.
@property BOOL shortcutsRunCommandsEnabled;
@property (null_resettable) NSString *fontFamily;
@property (readonly) NSString *fontFamilyUserFacingName;
@property (readonly) UIFont *approximateFont;
@property NSNumber *fontSize;
// Cell height as a multiple of the font's measured maximum extent. 1 is that
// height unchanged, which is what every build before this one did.
@property NSNumber *lineHeight;
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
// The Wayland desktop's two size settings, which are deliberately separate.
//
// displayDesktopScale is how many desktop PIXELS the applet asks for per point
// of the surface showing it: 1 is one pixel per point (what this has always
// done), 2 and 3 ask for that many times as many, and 0 means the device's own
// nativeScale. More pixels cost quadratically -- wayvnc encodes every frame in
// software inside the guest -- so this stays at 1 unless asked.
//
// displayUIScale is the scale the compositor reports to its clients, which
// decides how big things LOOK. 0 means "match the resolution", which keeps text
// and windows the physical size they are now and just renders them sharply; 1
// with a raised resolution instead fits proportionally more on screen at
// proportionally smaller size. Applied in the guest with wlr-randr.
@property NSInteger displayDesktopScale;
@property NSInteger displayUIScale;
// When YES, AOK never writes the guest's /etc/resolv.conf and never binds the
// local DNS responder on 127.0.0.1:53, leaving name resolution entirely to the
// guest. The default (NO) rewrites the file on every network path change, which
// is what most roots want -- but a root running its own resolver (dnsmasq,
// systemd-resolved, unbound) owns that file and wants that port, and the
// rewrite bumps the file's mtime on every cellular path update, which those
// resolvers poll for. Checked before customDnsServers, so a list left behind in
// preferences cannot resurrect the rewrite.
@property BOOL shouldDisableResolvConfRewrite;
// When YES, new terminal sessions launched with the default "/bin/login -f root" command log in
// as the unprivileged UID 1000 account instead, matching how a real Linux desktop is normally
// used. Sessions with a customized launch command are unaffected. The headless command surfaces
// (LLM chat's run_shell tool, the Shortcuts Run Command intent) and the Display applet's Wayland
// session follow the same rule via su -- see +[AppDelegate headlessCommandAccountName] and
// DisplayGuestSessionCommand. Session Shell / System Console surfaces stay root by design.
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

// The largest swap area the UI will offer or store, in MiB. 16 GiB: the slot
// table costs 1 MiB per GiB of swap (docs/simulated_swap_plan.md section 3.14),
// so this is 16 MiB of always-resident bookkeeping at the top of the range,
// and the area is preallocated in the container, so a bigger number is a
// bigger hole in the user's free space rather than a bigger win.
extern const NSInteger ISHSwapMaxSizeMB;
extern const NSInteger ISHCompressedMemoryMaxSizeMB;
// The ceiling actually enforced: a quarter of this device's RAM, floored at
// 64 MB and capped at ISHCompressedMemoryMaxSizeMB. The pool is resident
// memory and competes with what it saves, so a fixed number is wrong on a
// small device -- see the definition.
NSInteger ISHCompressedMemoryMaxForDevice(void);

// "Open Everything as Default User" targets whatever account this rootfs already has at UID
// 1000 (the conventional "first regular user" UID on Debian/Devuan/Alpine) -- no account is
// provisioned/created by iSH itself. See +[AppDelegate defaultUserAccountName], which looks up
// the actual account name (or returns nil if none exists) at each use.
extern const int ISHDefaultUserAccountUID;

NS_ASSUME_NONNULL_END
