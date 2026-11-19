//
//  MarkdownRenderer.h
//  AOK-iSH
//
//  A small, dependency-free Markdown -> NSAttributedString renderer, shared by
//  the LLM chat transcript and the Workspace markdown reader applet.
//  NSAttributedString's own initWithMarkdown... is iOS 15+ and inline only
//  (no headings/lists/code blocks), and the app targets iOS 12, so this
//  handles the block- and inline-level constructs Markdown documents and LLM
//  replies actually use: headings, bold/italic, inline code, fenced code
//  blocks, blockquotes, bullet/numbered lists, links, and horizontal rules.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

// Applies `add` on top of font's existing symbolic traits (e.g. bold a font
// that's already italic without losing the italic).
UIFont *ISHMarkdownFontWithTraits(UIFont *font, UIFontDescriptorSymbolicTraits add);

// Renders `markdown` with the given palette. `codeBg` may be nil (no code
// block / inline-code background fill).
NSAttributedString *ISHMarkdownAttributedStringFromMarkdown(NSString *markdown,
                                                              UIFont *baseFont,
                                                              UIColor *textColor,
                                                              UIColor *secondaryColor,
                                                              UIColor * _Nullable codeBg,
                                                              UIColor *linkColor);

typedef NS_ENUM(NSInteger, ISHMarkdownBlockKind) {
    ISHMarkdownBlockKindText,
    ISHMarkdownBlockKindCode,
};

// One top-level segment of a message: either prose (rendered, wraps normally)
// or a single fenced code block (raw text + the language tag from the
// opening fence, meant to be shown in its own non-wrapping, horizontally
// scrolling view with a Copy button -- code just doesn't read right wrapped).
@interface ISHMarkdownBlock : NSObject
@property (nonatomic, readonly) ISHMarkdownBlockKind kind;
@property (nonatomic, readonly, nullable) NSAttributedString *attributedText; // kind == Text
@property (nonatomic, readonly, nullable) NSString *code;                    // kind == Code
@property (nonatomic, readonly, nullable) NSString *language;                // kind == Code, may be ""
@end

// Splits `markdown` into an ordered list of text/code blocks. Each text
// segment is rendered with ISHMarkdownAttributedStringFromMarkdown (codeBg
// nil -- code segments carry their own presentation), so this is a pure
// pre-split, not a second parser.
NSArray<ISHMarkdownBlock *> *ISHMarkdownBlocksFromMarkdown(NSString *markdown,
                                                            UIFont *baseFont,
                                                            UIColor *textColor,
                                                            UIColor *secondaryColor,
                                                            UIColor *linkColor);

// Wraps `text` verbatim (no Markdown parsing) as a single text block -- for
// content that must never be reinterpreted, like the user's own prompt.
ISHMarkdownBlock *ISHMarkdownPlainTextBlock(NSString *text, UIFont *font, UIColor *color);

NS_ASSUME_NONNULL_END
