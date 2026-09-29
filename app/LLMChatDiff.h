//
//  LLMChatDiff.h
//  iSH-AOK
//
//  Line diffs for the LLM Chat's change review (Changes… and /undo).
//  Foundation only, tested on the host (tests/unit/llm_diff_test.m).
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, ISHLLMDiffLineKind) {
    ISHLLMDiffLineContext,
    ISHLLMDiffLineRemoved,
    ISHLLMDiffLineAdded,
    ISHLLMDiffLineHunkHeader, // "@@ -a,b +c,d @@"
};

@interface ISHLLMDiffLine : NSObject
@property (nonatomic, readonly) ISHLLMDiffLineKind kind;
@property (nonatomic, copy, readonly) NSString *text; // without the leading " ", "-" or "+"
@end

// Splits at "\n"; a final newline does not make an empty last line.
NSArray<NSString *> *ISHLLMDiffSplitLines(NSString *text);

// The shortest edit script between two line arrays (Myers), grouped into
// unified-diff hunks with `context` unchanged lines around each change.
// Empty when the two are equal. Past maxEdits differing lines it gives up
// and returns nil (a rewrite that large is better shown as "replaced").
NSArray<ISHLLMDiffLine *> *_Nullable ISHLLMUnifiedDiff(NSArray<NSString *> *before, NSArray<NSString *> *after,
                                                       NSUInteger context, NSUInteger maxEdits);

// Lines removed and added, for a one-line summary ("-3 +5").
void ISHLLMDiffCounts(NSArray<ISHLLMDiffLine *> *diff, NSUInteger *removed, NSUInteger *added);

// The diff as plain unified-diff text, one line per entry.
NSString *ISHLLMDiffText(NSArray<ISHLLMDiffLine *> *diff);

NS_ASSUME_NONNULL_END
