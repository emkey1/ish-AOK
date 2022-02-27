//
//  Theme.m
//  iSH
//
//  Created by Saagar Jha on 2/25/22.
//

#import "Theme.h"
#import "UserPreferences.h"

@implementation UIColor (iSH)
- (nullable instancetype)initWithHexString:(NSString *)string {
    NSScanner *scanner = [NSScanner scannerWithString:string];
    // Skip the leading #
    [scanner setScanLocation:1];
    unsigned int value;
    if (![scanner scanHexInt:&value]) {
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

@end

@interface Theme ()
@property(readonly) BOOL isDark;
@end

@implementation Theme {
    
}

- (instancetype)initWithName:(NSString *)name palette:(Palette *)palette {
    return [self initWithName:name lightPalette:palette darkPalette:palette];
}

- (instancetype)initWithName:(NSString *)name lightPalette:(nonnull Palette *)lightPalette darkPalette:(nonnull Palette *)darkPalette {
    if (self = [super init]) {
        self->_name = name;
        self->_lightPalette = lightPalette;
        self->_darkPalette = darkPalette;
    }
    return self;
}

+ (void)initialize {
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, 0, DISPATCH_VNODE_WRITE, dispatch_get_main_queue());
    dispatch_source_set_event_handler(source, ^{
        
    });
    dispatch_activate(source);
}

+ (NSArray<Theme *> *)defaultThemes {
    static NSArray<Theme *> *defaultThemes;
    if (!defaultThemes) {
        defaultThemes = @[
            [[self alloc] initWithName:@"Default"
                          lightPalette:[[Palette alloc] initWithForegroundColor:@"#000"
                                                                backgroundColor:@"#fff"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:nil]
                           darkPalette:[[Palette alloc] initWithForegroundColor:@"#fff"
                                                                backgroundColor:@"#000"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:nil]],
            [[self alloc] initWithName:@"1337"
                               palette:[[Palette alloc] initWithForegroundColor:@"#0f0"
                                                                backgroundColor:@"#000"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:nil]],
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
                            @"#002b36",
                            @"#cb4b16",
                            @"#586e75",
                            @"#657b83",
                            @"#839496",
                            @"#6c71c4",
                            @"#93a1a1",
                            @"#fdf6e3",
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
                            @"#002b36",
                            @"#cb4b16",
                            @"#586e75",
                            @"#657b83",
                            @"#839496",
                            @"#6c71c4",
                            @"#93a1a1",
                            @"#fdf6e3",
                           ]]
            ],
            // Because this is a hidden theme, it needs to be last. There's
            // logic in UserPreferences and ThemesViewController which will not
            // work correctly otherwise.
            [[self alloc] initWithName:@"Hot Dog Stand"
                               palette:[[Palette alloc] initWithForegroundColor:@"#ff0"
                                                                backgroundColor:@"#f00"
                                                                    cursorColor:nil
                                                          colorPaletteOverrides:nil]],
        ];
    }
    return defaultThemes;
}

- (BOOL)isDark {
    return NO;
}

- (UIStatusBarStyle)statusBarStyle {
    if (self.isDark) {
        return UIStatusBarStyleLightContent;
    } else {
        if (@available(iOS 13.0, *)) {
            return UIStatusBarStyleDarkContent;
        } else {
            return UIStatusBarStyleDefault;
        }
    }
}

@end
