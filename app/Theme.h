//
//  Theme.h
//  iSH
//
//  Created by Saagar Jha on 2/25/22.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface Palette : NSObject
@property(readonly) NSString *foregroundColor;
@property(readonly) NSString *backgroundColor;
@property(readonly, nullable) NSString *cursorColor;
@property(readonly, nullable) NSArray<NSString *> *colorPaletteOverrides;

- (instancetype)initWithForegroundColor:(NSString *)foregroundColor backgroundColor:(NSString *)backgroundColor cursorColor:(nullable NSString *)cursorColor colorPaletteOverrides:(nullable NSArray<NSString *> *)colorPaletteOverrides;

@end

@interface Theme : NSObject
@property(class, readonly) NSArray<Theme *> *defaultThemes;

@property(readonly) NSString *name;
@property(readonly) UIStatusBarStyle statusBarStyle;
@property(readonly) Palette *lightPalette;
@property(readonly) Palette *darkPalette;

- (instancetype)initWithName:(NSString *)name palette:(Palette *)palette;
- (instancetype)initWithName:(NSString *)name lightPalette:(Palette *)lightPalette darkPalette:(Palette *)darkPalette;
@end

@interface UIColor (iSH)
- (nullable instancetype)initWithHexString:(NSString *)string;
@end

NS_ASSUME_NONNULL_END
