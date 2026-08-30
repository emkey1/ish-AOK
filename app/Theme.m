//
//  Theme.m
//  iSH
//
//  Created by Saagar Jha on 2/25/22.
//

#import "Theme.h"
#import "UserPreferences.h"
#import "fs/proc/ish.h"

char *get_documents_directory_impl(void) {
    return strdup(NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject.UTF8String);
}

#define THEME_VERSION 1

@implementation UIColor (iSH)
- (nullable instancetype)ish_initWithHexString:(NSString *)string {
    if (![string hasPrefix:@"#"]) {
        return nil;
    }
    NSScanner *scanner = [NSScanner scannerWithString:string];
    // Skip the leading #
    [scanner setScanLocation:1];
    unsigned int value;
    if (![scanner scanHexInt:&value] || scanner.scanLocation != string.length) {
        return nil;
    }
    unsigned int red;
    unsigned int green;
    unsigned int blue;
    unsigned int alpha;
    if (string.length == 4) { // RGB
        blue = ((value & 0x00f) >> 0) * 0x11;
        green = ((value & 0x0f0) >> 4) * 0x11;
        red = ((value & 0xf00) >> 8) * 0x11;
        alpha = 0xff;
    } else if (string.length == 5) { // RGBA
        blue = ((value & 0x000f) >> 0) * 0x11;
        green = ((value & 0x00f0) >> 4) * 0x11;
        red = ((value & 0x0f00) >> 8) * 0x11;
        alpha = ((value & 0xf000) >> 12) * 0x11;
    } else if (string.length == 7) { // RRGGBB
        blue = (value & 0x0000ff) >> 0;
        green = (value & 0x00ff00) >> 8;
        red = (value & 0xff0000) >> 16;
        alpha = 0xff;
    } else if (string.length == 9) { // RRGGBBAA
        blue = (value & 0x000000ff) >> 0;
        green = (value & 0x0000ff00) >> 8;
        red = (value & 0x00ff0000) >> 16;
        alpha = (value & 0xff000000) >> 24;
    } else {
        return nil;
    }
    return [UIColor colorWithRed:1.0 * red / 0xff green:1.0 * green / 0xff blue:1.0 * blue / 0xff alpha:1.0 * alpha / 0xff];
}
@end

@interface DirectoryWatcher: NSObject<NSFilePresenter>
@property(readonly, copy) NSURL *presentedItemURL;
- (instancetype)initWithURL:(NSURL *)url handler:(void (^)(void))handler;
@end

@implementation DirectoryWatcher {
    void (^_handler)(void);
}
- (instancetype)initWithURL:(NSURL *)url handler:(void (^)(void))handler {
    if (self = [super init]) {
        self->_presentedItemURL = url;
        self->_handler = handler;
    }
    return self;
}

- (NSOperationQueue *)presentedItemOperationQueue {
    return NSOperationQueue.mainQueue;
}

- (void)presentedItemDidChange {
    self->_handler();
}
@end

@interface Palette ()
@property(readonly, nonnull) NSDictionary *serializedRepresentation;

- (nullable instancetype)initWithSerializedRepresentation:(nonnull NSDictionary *)serializedRepresentation;
@end

@implementation Palette

- (instancetype)initWithForegroundColor:(NSString *)foregroundColor backgroundColor:(NSString *)backgroundColor cursorColor:(NSString *)cursorColor colorPaletteOverrides:(NSArray<NSString *> *)colorPaletteOverrides {
    if (self = [super init]) {
        self->_foregroundColor = foregroundColor;
        self->_backgroundColor = backgroundColor;
        self->_cursorColor = cursorColor;
        self->_colorPaletteOverrides = colorPaletteOverrides;
    }
    return self;
}

- (instancetype)initWithSerializedRepresentation:(NSDictionary *)serializedRepresentation {
#define VALID_COLOR(color) (color && [color isKindOfClass:NSString.class] && [[UIColor alloc] ish_initWithHexString:color])
    id foregroundColor = serializedRepresentation[@"foregroundColor"];
    id backgroundColor = serializedRepresentation[@"backgroundColor"];
    id cursorColor = serializedRepresentation[@"cursorColor"];
    id colorPaletteOverrides = serializedRepresentation[@"colorPaletteOverrides"];
    BOOL validColorPalette = YES;
    if (colorPaletteOverrides) {
        if ([colorPaletteOverrides isKindOfClass:NSArray.class]) {
            for (id color in colorPaletteOverrides) {
                validColorPalette = validColorPalette && VALID_COLOR(color);
            }
        } else {
            validColorPalette = NO;
        }
    }
    if (VALID_COLOR(foregroundColor) && VALID_COLOR(backgroundColor) && (!cursorColor || VALID_COLOR(cursorColor)) && validColorPalette) {
        return [self initWithForegroundColor:foregroundColor backgroundColor:backgroundColor cursorColor:cursorColor colorPaletteOverrides:colorPaletteOverrides];
    } else {
        return nil;
    }
#undef VALID_COLOR
}

- (NSDictionary *)serializedRepresentation {
    NSMutableDictionary *representation = [@{
        @"foregroundColor": self.foregroundColor,
        @"backgroundColor": self.backgroundColor,
    } mutableCopy];
    if (self.cursorColor) {
        representation[@"cursorColor"] = self.cursorColor;
    }
    if (self.colorPaletteOverrides) {
        representation[@"colorPaletteOverrides"] = self.colorPaletteOverrides;
    }
    return  representation;
}

@end

@interface ThemeAppearance ()
@property(readonly, nonnull) NSDictionary *serializedRepresentation;

- (nullable instancetype)initWithSerializedRepresentation:(nonnull NSDictionary *)serializedRepresentation;
@end

@implementation ThemeAppearance

- (instancetype)initWithLightOverride:(BOOL)lightOverride darkOverride:(BOOL)darkOverride {
    if (self = [super init]) {
        self->_lightOverride = lightOverride;
        self->_darkOverride = darkOverride;
    }
    return self;
}

- (instancetype)initWithSerializedRepresentation:(NSDictionary *)serializedRepresentation {
    id lightOverride = serializedRepresentation[@"lightOverride"];
    id darkOverride = serializedRepresentation[@"darkOverride"];
    if ([lightOverride isKindOfClass:NSNumber.class] && [darkOverride isKindOfClass:NSNumber.class]) {
        return [self initWithLightOverride:[lightOverride boolValue] darkOverride:[darkOverride boolValue]];
    } else {
        return nil;
    }
}

+ (instancetype)alwaysLight {
    return [[self alloc] initWithLightOverride:NO darkOverride:YES];
}

+ (instancetype)alwaysDark {
    return [[self alloc] initWithLightOverride:YES darkOverride:NO];
}

- (NSDictionary *)serializedRepresentation {
    return @{
        @"lightOverride": @(self.lightOverride),
        @"darkOverride": @(self.darkOverride),
    };
}

@end

DirectoryWatcher *directoryWatcher;
NSString *const ThemesUpdatedNotification = @"ThemesUpdatedNotification";
NSString *const ThemeUpdatedNotification = @"ThemeUpdatedNotification";

@interface Theme ()
@property(readonly, nonnull) NSData *data;
@end

// TODO: Move these to Linux

@implementation Theme
+ (void)initialize {
    directoryWatcher = [[DirectoryWatcher alloc] initWithURL:self.themesDirectory handler:^{
        [NSNotificationCenter.defaultCenter postNotificationName:ThemesUpdatedNotification object:nil];
    }];
    [NSFileCoordinator addFilePresenter:directoryWatcher];
    
    get_documents_directory = get_documents_directory_impl;
    [NSFileManager.defaultManager createDirectoryAtURL:self.themesDirectory withIntermediateDirectories:YES attributes:nil error:nil];
}

- (instancetype)initWithName:(NSString *)name palette:(Palette *)palette appearance:(ThemeAppearance *)appearance {
    Theme *theme = [self initWithName:name lightPalette:palette darkPalette:palette appearance:appearance];
    return theme;
}

- (instancetype)initWithName:(NSString *)name lightPalette:(nonnull Palette *)lightPalette darkPalette:(nonnull Palette *)darkPalette appearance:(nullable ThemeAppearance *)appearance {
    if (self = [super init]) {
        self->_name = name;
        self->_lightPalette = lightPalette;
        self->_darkPalette = darkPalette;
        self->_appearance = appearance;
    }
    return self;
}

- (nullable instancetype)initWithName:(NSString *)name data:(NSData *)data {
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![json isKindOfClass:NSDictionary.class]) {
        return nil;
    }
       id version = json[@"version"];
    if (![version isKindOfClass:NSNumber.class] || ((NSNumber *)version).integerValue <= 0 || ((NSNumber *)version).integerValue > THEME_VERSION) {
        NSLog(@"Rejecting theme %@ with invalid version number", name);
        return nil;
    }
    id _appearance = json[@"appearance"];
    ThemeAppearance *appearance = [_appearance isKindOfClass:NSDictionary.class] ? [[ThemeAppearance alloc] initWithSerializedRepresentation:_appearance] : nil;
    id shared = json[@"shared"];
    id light = json[@"light"];
    id dark = json[@"dark"];
    if ([shared isKindOfClass:NSDictionary.class]) {
        Palette *palette = [[Palette alloc] initWithSerializedRepresentation:shared];
        return palette ? [self initWithName:name palette:palette appearance:appearance] : nil;
    } else if ([light isKindOfClass:NSDictionary.class] && [dark isKindOfClass:NSDictionary.class]) {
        Palette *lightPalette = [[Palette alloc] initWithSerializedRepresentation:light];
        Palette *darkPalette = [[Palette alloc] initWithSerializedRepresentation:dark];
        return lightPalette && darkPalette ? [self initWithName:name lightPalette:lightPalette darkPalette:darkPalette appearance:appearance] : nil;
    } else {
        NSLog(@"Rejecting theme %@ with invalid palette(s)", name);
        return nil;
    }
}

// A preset's sixteen ANSI colors always arrive as one ordered run -- black,
// red, green, yellow, blue, magenta, cyan, white, then the eight bright
// counterparts -- which is how every upstream palette publishes them. Spelling
// each entry out the long way would triple the size of +defaultThemes for no
// added clarity, and the one thing worth checking mechanically is that a
// palette carries all sixteen: hterm silently ignores a short override array,
// so a missing color would show up as "that theme looks slightly off" rather
// than as a failure.
static Palette *paletteWithColors(NSString *foregroundColor, NSString *backgroundColor, NSArray<NSString *> *ansiColors) {
    NSCAssert(ansiColors.count == 16, @"a palette override needs all sixteen ANSI colors");
    return [[Palette alloc] initWithForegroundColor:foregroundColor
                                    backgroundColor:backgroundColor
                                        cursorColor:nil
                              colorPaletteOverrides:ansiColors];
}

+ (NSArray<Theme *> *)defaultThemes {
    static NSArray<Theme *> *defaultThemes;
    if (!defaultThemes) {
        defaultThemes = @[
            [[self alloc] initWithName:@"Amber"
                          lightPalette:[[Palette alloc] initWithForegroundColor:@"#FD9F20"
                                                                backgroundColor:@"#fff"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:nil]
                           darkPalette:[[Palette alloc] initWithForegroundColor:@"#FD9F20"
                                                                backgroundColor:@"#000"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:nil]
                            appearance:nil],
            [[self alloc] initWithName:@"iSH-Default"
                          lightPalette:[[Palette alloc] initWithForegroundColor:@"#000"
                                                                backgroundColor:@"#fff"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:nil]
                           darkPalette:[[Palette alloc] initWithForegroundColor:@"#fff"
                                                                backgroundColor:@"#000"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:nil]
                            appearance:nil],
            [[self alloc] initWithName:@"1337"
                               palette:[[Palette alloc] initWithForegroundColor:@"#0f0"
                                                                backgroundColor:@"#000"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:nil]
                            appearance:ThemeAppearance.alwaysDark],
            [[self alloc] initWithName:@"Solarized"
                          lightPalette:[[Palette alloc] initWithForegroundColor:@"#657b83"
                                                                backgroundColor:@"#fdf6e3"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:@[
                            @"#073642",
                            @"#dc322f",
                            @"#859900",
                            @"#b58900",
                            @"#268bd2",
                            @"#d33682",
                            @"#2aa198",
                            @"#eee8d5",
                            @"#586e75",
                            @"#cb4b16",
                            @"#586e75",
                            @"#657b83",
                            @"#839496",
                            @"#6c71c4",
                            @"#93a1a1",
                            @"#93a1a1",
                          ]]
                           darkPalette:[[Palette alloc] initWithForegroundColor:@"#839496"
                                                                backgroundColor:@"#002b36"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:@[
                            @"#073642",
                            @"#dc322f",
                            @"#859900",
                            @"#b58900",
                            @"#268bd2",
                            @"#d33682",
                            @"#2aa198",
                            @"#eee8d5",
                            @"#586e75",
                            @"#cb4b16",
                            @"#586e75",
                            @"#657b83",
                            @"#839496",
                            @"#6c71c4",
                            @"#93a1a1",
                            @"#fdf6e3",
                           ]]
                            appearance:nil
            ],
            // Well-known terminal palettes, transcribed from each project's own
            // published values -- an approximate Nord is worse than no Nord,
            // because people recognize these by sight. Themes whose upstream
            // ships both a dark and a light variant get both, so switching
            // appearance stays inside the theme instead of falling back to
            // something that clashes. Dark-only palettes carry a single palette
            // and .alwaysDark, the same shape 1337 uses: the terminal keeps the
            // colors it was designed for and the surrounding UI matches it,
            // rather than a dark palette sitting under a light keyboard.
            // Catppuccin Mocha / Latte -- github.com/catppuccin/alacritty
            [[self alloc] initWithName:@"Catppuccin"
                          lightPalette:paletteWithColors(@"#4c4f69", @"#eff1f5",
                                                         @[@"#bcc0cc", @"#d20f39", @"#40a02b", @"#df8e1d", @"#1e66f5", @"#ea76cb", @"#179299", @"#5c5f77",
                                                           @"#acb0be", @"#d20f39", @"#40a02b", @"#df8e1d", @"#1e66f5", @"#ea76cb", @"#179299", @"#6c6f85"])
                           darkPalette:paletteWithColors(@"#cdd6f4", @"#1e1e2e",
                                                        @[@"#45475a", @"#f38ba8", @"#a6e3a1", @"#f9e2af", @"#89b4fa", @"#f5c2e7", @"#94e2d5", @"#bac2de",
                                                          @"#585b70", @"#f38ba8", @"#a6e3a1", @"#f9e2af", @"#89b4fa", @"#f5c2e7", @"#94e2d5", @"#a6adc8"])
                            appearance:nil],
            // Dracula -- github.com/dracula/alacritty
            [[self alloc] initWithName:@"Dracula"
                               palette:paletteWithColors(@"#f8f8f2", @"#282a36",
                                                        @[@"#21222c", @"#ff5555", @"#50fa7b", @"#f1fa8c", @"#bd93f9", @"#ff79c6", @"#8be9fd", @"#f8f8f2",
                                                          @"#6272a4", @"#ff6e6e", @"#69ff94", @"#ffffa5", @"#d6acff", @"#ff92df", @"#a4ffff", @"#ffffff"])
                            appearance:ThemeAppearance.alwaysDark],
            // Everforest Dark/Light Medium -- sainnhe/everforest palette.md (upstream
            // maps bright to normal for terminal colors, so these do too)
            [[self alloc] initWithName:@"Everforest"
                          lightPalette:paletteWithColors(@"#5c6a72", @"#fdf6e3",
                                                         @[@"#5c6a72", @"#f85552", @"#8da101", @"#dfa000", @"#3a94c5", @"#df69ba", @"#35a77c", @"#e0dcc7",
                                                           @"#5c6a72", @"#f85552", @"#8da101", @"#dfa000", @"#3a94c5", @"#df69ba", @"#35a77c", @"#e0dcc7"])
                           darkPalette:paletteWithColors(@"#d3c6aa", @"#2d353b",
                                                        @[@"#475258", @"#e67e80", @"#a7c080", @"#dbbc7f", @"#7fbbb3", @"#d699b6", @"#83c092", @"#d3c6aa",
                                                          @"#475258", @"#e67e80", @"#a7c080", @"#dbbc7f", @"#7fbbb3", @"#d699b6", @"#83c092", @"#d3c6aa"])
                            appearance:nil],
            // Gruvbox Dark/Light -- github.com/morhetz/gruvbox
            [[self alloc] initWithName:@"Gruvbox"
                          lightPalette:paletteWithColors(@"#3c3836", @"#fbf1c7",
                                                         @[@"#fbf1c7", @"#cc241d", @"#98971a", @"#d79921", @"#458588", @"#b16286", @"#689d6a", @"#7c6f64",
                                                           @"#928374", @"#9d0006", @"#79740e", @"#b57614", @"#076678", @"#8f3f71", @"#427b58", @"#3c3836"])
                           darkPalette:paletteWithColors(@"#ebdbb2", @"#282828",
                                                        @[@"#282828", @"#cc241d", @"#98971a", @"#d79921", @"#458588", @"#b16286", @"#689d6a", @"#a89984",
                                                          @"#928374", @"#fb4934", @"#b8bb26", @"#fabd2f", @"#83a598", @"#d3869b", @"#8ec07c", @"#ebdbb2"])
                            appearance:nil],
            // Kanagawa Wave / Lotus -- github.com/rebelot/kanagawa.nvim
            [[self alloc] initWithName:@"Kanagawa"
                          lightPalette:paletteWithColors(@"#545464", @"#f2ecbc",
                                                         @[@"#1f1f28", @"#c84053", @"#6f894e", @"#77713f", @"#4d699b", @"#b35b79", @"#597b75", @"#545464",
                                                           @"#8a8980", @"#d7474b", @"#6e915f", @"#836f4a", @"#6693bf", @"#624c83", @"#5e857a", @"#43436c"])
                           darkPalette:paletteWithColors(@"#dcd7ba", @"#1f1f28",
                                                        @[@"#090618", @"#c34043", @"#76946a", @"#c0a36e", @"#7e9cd8", @"#957fb8", @"#6a9589", @"#c8c093",
                                                          @"#727169", @"#e82424", @"#98bb6c", @"#e6c384", @"#7fb4ca", @"#938aa9", @"#7aa89f", @"#dcd7ba"])
                            appearance:nil],
            // Nord -- github.com/nordtheme/alacritty
            [[self alloc] initWithName:@"Nord"
                               palette:paletteWithColors(@"#d8dee9", @"#2e3440",
                                                        @[@"#3b4252", @"#bf616a", @"#a3be8c", @"#ebcb8b", @"#81a1c1", @"#b48ead", @"#88c0d0", @"#e5e9f0",
                                                          @"#4c566a", @"#bf616a", @"#a3be8c", @"#ebcb8b", @"#81a1c1", @"#b48ead", @"#8fbcbb", @"#eceff4"])
                            appearance:ThemeAppearance.alwaysDark],
            // One Dark -- github.com/alacritty/alacritty-theme
            [[self alloc] initWithName:@"One Dark"
                               palette:paletteWithColors(@"#abb2bf", @"#282c34",
                                                        @[@"#1e2127", @"#e06c75", @"#98c379", @"#d19a66", @"#61afef", @"#c678dd", @"#56b6c2", @"#abb2bf",
                                                          @"#5c6370", @"#e06c75", @"#98c379", @"#d19a66", @"#61afef", @"#c678dd", @"#56b6c2", @"#ffffff"])
                            appearance:ThemeAppearance.alwaysDark],
            // Rosé Pine / Rosé Pine Dawn -- github.com/rose-pine/alacritty
            [[self alloc] initWithName:@"Rosé Pine"
                          lightPalette:paletteWithColors(@"#575279", @"#faf4ed",
                                                         @[@"#f2e9e1", @"#b4637a", @"#286983", @"#ea9d34", @"#56949f", @"#907aa9", @"#d7827e", @"#575279",
                                                           @"#9893a5", @"#b4637a", @"#286983", @"#ea9d34", @"#56949f", @"#907aa9", @"#d7827e", @"#575279"])
                           darkPalette:paletteWithColors(@"#e0def4", @"#191724",
                                                        @[@"#26233a", @"#eb6f92", @"#31748f", @"#f6c177", @"#9ccfd8", @"#c4a7e7", @"#ebbcba", @"#e0def4",
                                                          @"#6e6a86", @"#eb6f92", @"#31748f", @"#f6c177", @"#9ccfd8", @"#c4a7e7", @"#ebbcba", @"#e0def4"])
                            appearance:nil],
            // Tokyo Night / Tokyo Night Day -- github.com/folke/tokyonight.nvim
            [[self alloc] initWithName:@"Tokyo Night"
                          lightPalette:paletteWithColors(@"#3760bf", @"#e1e2e7",
                                                         @[@"#b4b5b9", @"#f52a65", @"#587539", @"#8c6c3e", @"#2e7de9", @"#9854f1", @"#007197", @"#6172b0",
                                                           @"#a1a6c5", @"#ff4774", @"#5c8524", @"#a27629", @"#358aff", @"#a463ff", @"#007ea8", @"#3760bf"])
                           darkPalette:paletteWithColors(@"#c0caf5", @"#1a1b26",
                                                        @[@"#15161e", @"#f7768e", @"#9ece6a", @"#e0af68", @"#7aa2f7", @"#bb9af7", @"#7dcfff", @"#a9b1d6",
                                                          @"#414868", @"#ff899d", @"#9fe044", @"#faba4a", @"#8db0ff", @"#c7a9ff", @"#a4daff", @"#c0caf5"])
                            appearance:nil],
            // Tokyo Night Storm -- github.com/folke/tokyonight.nvim
            [[self alloc] initWithName:@"Tokyo Night Storm"
                               palette:paletteWithColors(@"#c0caf5", @"#24283b",
                                                        @[@"#1d202f", @"#f7768e", @"#9ece6a", @"#e0af68", @"#7aa2f7", @"#bb9af7", @"#7dcfff", @"#a9b1d6",
                                                          @"#414868", @"#ff899d", @"#9fe044", @"#faba4a", @"#8db0ff", @"#c7a9ff", @"#a4daff", @"#c0caf5"])
                            appearance:ThemeAppearance.alwaysDark],
            // Because this is a hidden theme, it needs to be last. There's
            // logic in UserPreferences and ThemesViewController which will not
            // work correctly otherwise.
            [[self alloc] initWithName:@"Hot Dog Stand"
                               palette:[[Palette alloc] initWithForegroundColor:@"#ff0"
                                                                backgroundColor:@"#f00"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:nil]
                            appearance:nil],
        ];
    }
    return defaultThemes;
}

+ (NSURL *)themesDirectory {
    return [[NSURL fileURLWithPath:NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject] URLByAppendingPathComponent:@"themes"];
}

+ (NSArray<Theme *> *)userThemes {
    NSMutableArray<Theme *> *themes = [NSMutableArray new];
    for (NSURL *file in [NSFileManager.defaultManager contentsOfDirectoryAtURL:self.themesDirectory includingPropertiesForKeys:nil options:0 error:nil]) {
        Theme *theme = [[Theme alloc] initWithName:file.lastPathComponent.stringByDeletingPathExtension data:[NSData dataWithContentsOfURL:file]];
        if (theme) {
            [themes addObject:theme];
        }
    }
    [themes sortUsingDescriptors:@[[NSSortDescriptor sortDescriptorWithKey:@"name" ascending:YES selector:@selector(localizedStandardCompare:)]]];
    return themes;
}

- (NSData *)data {
    NSMutableDictionary *representation = [@{
        @"version": @(THEME_VERSION),
    } mutableCopy];
    if (self.lightPalette == self.darkPalette) {
        representation[@"shared"] = self.lightPalette.serializedRepresentation;
    } else {
        representation[@"light"] = self.lightPalette.serializedRepresentation;
        representation[@"dark"] = self.darkPalette.serializedRepresentation;
    }
    if (self.appearance) {
        representation[@"appearance"] = self.appearance.serializedRepresentation;
    }
    return [NSJSONSerialization dataWithJSONObject:representation options:NSJSONWritingSortedKeys | NSJSONWritingPrettyPrinted error:nil];
}

+ (Theme *)themeForName:(NSString *)name includingDefaultThemes:(BOOL)includingDefaultThemes {
    // We should pick user themes over default ones, if they have the same name.
    NSMutableArray<Theme *> *themes = [Theme.userThemes mutableCopy];
    if (includingDefaultThemes) {
        [themes addObjectsFromArray:Theme.defaultThemes];
    }
    for (Theme *theme in themes) {
        if ([theme.name isEqualToString:name]) {
            return theme;
        }
    }
    return nil;
}

- (void)duplicateAsUserTheme {
    NSString *name;
    for (int suffix = 1; [self.class themeForName:name = [NSString stringWithFormat:@"%@-%d", self.name, suffix] includingDefaultThemes:NO]; ++suffix);
    [self.data writeToURL:[self.class.themesDirectory URLByAppendingPathComponent:[name stringByAppendingString:@".json"]] atomically:YES];
}

- (BOOL)addUserTheme {
    if ([self.class themeForName:self.name includingDefaultThemes:NO]) {
        return NO;
    } else {
        [self.data writeToURL:[self.class.themesDirectory URLByAppendingPathComponent:[self.name stringByAppendingString:@".json"]] atomically:YES];
        return YES;
    }
}

- (void)deleteUserTheme {
    [NSFileManager.defaultManager removeItemAtURL:[self.class.themesDirectory URLByAppendingPathComponent:[self.name stringByAppendingString:@".json"]] error:nil];
}

- (void)replaceWithUserTheme:(Theme *)theme {
    [theme.data writeToURL:[self.class.themesDirectory URLByAppendingPathComponent:[theme.name stringByAppendingString:@".json"]] atomically:YES];
    if (![self.name isEqualToString:theme.name]) {
        [self deleteUserTheme];
        [NSNotificationCenter.defaultCenter postNotificationName:ThemeUpdatedNotification object:theme.name];
    }
}
@end
