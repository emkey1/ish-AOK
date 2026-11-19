//
//  MarkdownRenderer.m
//  AOK-iSH
//

#import "MarkdownRenderer.h"

UIFont *ISHMarkdownFontWithTraits(UIFont *font, UIFontDescriptorSymbolicTraits add) {
    UIFontDescriptor *base = font.fontDescriptor;
    UIFontDescriptor *desc = [base fontDescriptorWithSymbolicTraits:(base.symbolicTraits | add)];
    return desc ? [UIFont fontWithDescriptor:desc size:font.pointSize] : font;
}

static UIFont *ISHMarkdownMonospaceFont(CGFloat size) {
    if (@available(iOS 13.0, *))
        return [UIFont monospacedSystemFontOfSize:size weight:UIFontWeightRegular];
    return [UIFont fontWithName:@"Menlo" size:size] ?: [UIFont systemFontOfSize:size];
}

// Splits a pipe-table row into trimmed cells, dropping the empty boundary
// cells a leading/trailing "|" produces.
static NSArray<NSString *> *ISHMarkdownTableCells(NSString *row) {
    NSCharacterSet *ws = NSCharacterSet.whitespaceCharacterSet;
    NSString *trimmed = [row stringByTrimmingCharactersInSet:ws];
    if ([trimmed hasPrefix:@"|"]) trimmed = [trimmed substringFromIndex:1];
    if ([trimmed hasSuffix:@"|"]) trimmed = [trimmed substringToIndex:trimmed.length - 1];
    NSMutableArray<NSString *> *cells = [NSMutableArray array];
    for (NSString *cell in [trimmed componentsSeparatedByString:@"|"])
        [cells addObject:[cell stringByTrimmingCharactersInSet:ws]];
    return cells;
}

// A table's second line is the header separator: only pipes, colons, dashes,
// and whitespace, with at least one dash ("|---|:--:|").
static BOOL ISHMarkdownLineIsTableSeparator(NSString *line) {
    NSString *trimmed = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (trimmed.length == 0 || ![trimmed containsString:@"-"])
        return NO;
    static NSCharacterSet *allowed;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        allowed = [NSCharacterSet characterSetWithCharactersInString:@"|:- \t"];
    });
    return [trimmed rangeOfCharacterFromSet:allowed.invertedSet].location == NSNotFound;
}

// Inline pass: emphasis (** * __ _), inline code (`), and links [text](url).
// Recurses for nested emphasis. Underscore emphasis is suppressed intra-word so
// snake_case and __dunder__ identifiers survive untouched.
static void ISHMarkdownAppendInline(NSMutableAttributedString *out, NSString *text, UIFont *font, NSDictionary *ctx) {
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
                    ISHMarkdownAppendInline(out, inner, ISHMarkdownFontWithTraits(font, UIFontDescriptorTraitBold), ctx);
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
                        ISHMarkdownAppendInline(out, inner, ISHMarkdownFontWithTraits(font, UIFontDescriptorTraitItalic), ctx);
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

NSAttributedString *ISHMarkdownAttributedStringFromMarkdown(NSString *markdown, UIFont *baseFont, UIColor *textColor, UIColor *secondaryColor, UIColor *codeBg, UIColor *linkColor) {
    NSMutableAttributedString *out = [NSMutableAttributedString new];
    if (markdown.length == 0)
        return out;
    CGFloat baseSize = baseFont.pointSize;
    UIFont *codeFont = ISHMarkdownMonospaceFont(baseSize - 1.0);
    NSMutableDictionary *ctx = [@{@"color": textColor, @"codeFont": codeFont, @"codeColor": textColor, @"linkColor": linkColor} mutableCopy];
    if (codeBg != nil)
        ctx[@"codeBg"] = codeBg; // nil means "no code-block/inline-code background fill" -- must not go in a dictionary literal
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

    for (NSUInteger lineIndex = 0; lineIndex < lines.count; lineIndex++) {
        NSString *rawLine = lines[lineIndex];
        NSString *trimmed = [rawLine stringByTrimmingCharactersInSet:ws];

        // fenced code block ``` or ~~~
        if ([trimmed hasPrefix:@"```"] || [trimmed hasPrefix:@"~~~"]) {
            if (inFence) { flushCode(); inFence = NO; } else { inFence = YES; }
            continue;
        }
        if (inFence) { [codeLines addObject:rawLine]; continue; }

        if (trimmed.length == 0) { appendNewline(); continue; }

        // pipe table: a header row containing "|" followed by a separator row
        // ("|---|---|"). Rendered as a monospaced grid -- column widths are
        // computed in characters, so ideographic/emoji-heavy cells can
        // misalign slightly; the tradeoff buys attributed-string tables with
        // no layout machinery. Cell text is rendered verbatim (no nested
        // emphasis), which is what keeps the columns honest.
        if ([trimmed containsString:@"|"] && lineIndex + 1 < lines.count &&
            ISHMarkdownLineIsTableSeparator(lines[lineIndex + 1])) {
            NSMutableArray<NSArray<NSString *> *> *tableRows = [NSMutableArray array];
            [tableRows addObject:ISHMarkdownTableCells(trimmed)];
            NSUInteger consumed = lineIndex + 2;  // skip the separator row
            while (consumed < lines.count && [lines[consumed] containsString:@"|"]) {
                [tableRows addObject:ISHMarkdownTableCells(lines[consumed])];
                consumed++;
            }
            lineIndex = consumed - 1;

            NSUInteger columnCount = 0;
            for (NSArray<NSString *> *row in tableRows)
                columnCount = MAX(columnCount, row.count);
            NSMutableArray<NSNumber *> *columnWidths = [NSMutableArray array];
            for (NSUInteger column = 0; column < columnCount; column++) {
                NSUInteger width = 0;
                for (NSArray<NSString *> *row in tableRows)
                    if (column < row.count) width = MAX(width, row[column].length);
                [columnWidths addObject:@(width)];
            }

            UIFont *headerFont = ISHMarkdownFontWithTraits(codeFont, UIFontDescriptorTraitBold);
            for (NSUInteger rowIndex = 0; rowIndex < tableRows.count; rowIndex++) {
                NSArray<NSString *> *row = tableRows[rowIndex];
                NSMutableString *renderedRow = [NSMutableString string];
                for (NSUInteger column = 0; column < columnCount; column++) {
                    NSString *cell = column < row.count ? row[column] : @"";
                    [renderedRow appendString:cell];
                    if (column + 1 < columnCount) {
                        NSUInteger pad = columnWidths[column].unsignedIntegerValue - cell.length + 2;
                        [renderedRow appendString:[@"" stringByPaddingToLength:pad withString:@" " startingAtIndex:0]];
                    }
                }
                [renderedRow appendString:@"\n"];
                [out appendAttributedString:[[NSAttributedString alloc] initWithString:renderedRow attributes:@{
                    NSFontAttributeName: (rowIndex == 0 ? headerFont : codeFont),
                    NSForegroundColorAttributeName: textColor,
                    NSParagraphStyleAttributeName: codeStyle,
                }]];
            }
            appendNewline();
            continue;
        }

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
            UIFont *hFont = ISHMarkdownFontWithTraits([baseFont fontWithSize:(CGFloat)round(baseSize * scale)], UIFontDescriptorTraitBold);
            NSUInteger start = out.length;
            ISHMarkdownAppendInline(out, headingText, hFont, ctx);
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
            ISHMarkdownAppendInline(out, quote, ISHMarkdownFontWithTraits(baseFont, UIFontDescriptorTraitItalic), qctx);
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
            [out appendAttributedString:[[NSAttributedString alloc] initWithString:marker attributes:@{NSFontAttributeName: (isNumbered ? ISHMarkdownFontWithTraits(baseFont, UIFontDescriptorTraitBold) : baseFont), NSForegroundColorAttributeName: textColor}]];
            ISHMarkdownAppendInline(out, item, baseFont, ctx);
            [out addAttribute:NSParagraphStyleAttributeName value:ls range:NSMakeRange(start, out.length - start)];
            appendNewline();
            continue;
        }

        // default paragraph line
        NSUInteger start = out.length;
        ISHMarkdownAppendInline(out, rawLine, baseFont, ctx);
        [out addAttribute:NSParagraphStyleAttributeName value:bodyStyle range:NSMakeRange(start, out.length - start)];
        appendNewline();
    }
    if (inFence && codeLines.count > 0)
        flushCode(); // unterminated fence (e.g. still streaming)

    return out;
}

@interface ISHMarkdownBlock ()
@property (nonatomic, readwrite) ISHMarkdownBlockKind kind;
@property (nonatomic, readwrite, nullable) NSAttributedString *attributedText;
@property (nonatomic, readwrite, nullable) NSString *code;
@property (nonatomic, readwrite, nullable) NSString *language;
@end

@implementation ISHMarkdownBlock
@end

NSArray<ISHMarkdownBlock *> *ISHMarkdownBlocksFromMarkdown(NSString *markdown, UIFont *baseFont, UIColor *textColor, UIColor *secondaryColor, UIColor *linkColor) {
    NSMutableArray<ISHMarkdownBlock *> *blocks = [NSMutableArray array];
    if (markdown.length == 0)
        return blocks;
    NSCharacterSet *ws = NSCharacterSet.whitespaceCharacterSet;
    NSArray<NSString *> *lines = [markdown componentsSeparatedByString:@"\n"];
    NSMutableArray<NSString *> *textLines = [NSMutableArray array];
    NSMutableArray<NSString *> *codeLines = [NSMutableArray array];
    __block NSString *codeLanguage = @"";
    BOOL inFence = NO;

    void (^flushText)(void) = ^{
        if (textLines.count == 0)
            return;
        NSString *chunk = [textLines componentsJoinedByString:@"\n"];
        [textLines removeAllObjects];
        if ([chunk stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length == 0)
            return;
        ISHMarkdownBlock *block = [ISHMarkdownBlock new];
        block.kind = ISHMarkdownBlockKindText;
        block.attributedText = ISHMarkdownAttributedStringFromMarkdown(chunk, baseFont, textColor, secondaryColor, nil, linkColor);
        [blocks addObject:block];
    };
    void (^flushCode)(void) = ^{
        NSString *code = [codeLines componentsJoinedByString:@"\n"];
        [codeLines removeAllObjects];
        ISHMarkdownBlock *block = [ISHMarkdownBlock new];
        block.kind = ISHMarkdownBlockKindCode;
        block.code = code;
        block.language = codeLanguage;
        [blocks addObject:block];
        codeLanguage = @"";
    };

    for (NSString *rawLine in lines) {
        NSString *trimmed = [rawLine stringByTrimmingCharactersInSet:ws];
        if ([trimmed hasPrefix:@"```"] || [trimmed hasPrefix:@"~~~"]) {
            if (inFence) {
                flushCode();
                inFence = NO;
            } else {
                flushText();
                codeLanguage = [[trimmed substringFromIndex:3] stringByTrimmingCharactersInSet:ws];
                inFence = YES;
            }
            continue;
        }
        if (inFence) { [codeLines addObject:rawLine]; continue; }
        [textLines addObject:rawLine];
    }
    if (inFence && codeLines.count > 0)
        flushCode(); // unterminated fence (e.g. still streaming)
    flushText();
    return blocks;
}

ISHMarkdownBlock *ISHMarkdownPlainTextBlock(NSString *text, UIFont *font, UIColor *color) {
    ISHMarkdownBlock *block = [ISHMarkdownBlock new];
    block.kind = ISHMarkdownBlockKindText;
    block.attributedText = [[NSAttributedString alloc] initWithString:text ?: @"" attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: color}];
    return block;
}
