//
//  LLMChatPermissions.h
//  iSH-AOK
//
//  Which of the LLM Chat's tool calls run on their own, which ask first, and
//  which are refused. Modelled on OpenCode's allow/ask/deny rules: every tool
//  belongs to a category with a default action, and shell commands can also
//  be matched against an ordered list of wildcard rules ("git status" allow,
//  "rm *" deny).
//
//  Foundation only, no UIKit and no guest access, so the rule logic (and the
//  glob tool's pattern matching) can be compiled and tested on its own
//  (tests/unit/llm_permissions_test.m).
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Ordered by severity: combining two answers keeps the larger one.
typedef NS_ENUM(NSInteger, ISHLLMPermissionAction) {
    ISHLLMPermissionAllow = 0,
    ISHLLMPermissionAsk = 1,
    ISHLLMPermissionDeny = 2,
};

typedef NS_ENUM(NSInteger, ISHLLMToolCategory) {
    ISHLLMToolCategoryRead,   // read_file, list_directory, glob, grep
    ISHLLMToolCategoryEdit,   // write_file, edit_file
    ISHLLMToolCategoryShell,  // run_shell
};

extern NSString *const kISHLLMShellRulePattern; // NSString, a wildcard pattern
extern NSString *const kISHLLMShellRuleAction;  // NSNumber, an ISHLLMPermissionAction

NSString *ISHLLMPermissionActionTitle(ISHLLMPermissionAction action);
NSString *ISHLLMToolCategoryTitle(ISHLLMToolCategory category);

// The saved settings (NSUserDefaults). Reads a missing or malformed value as
// the default: Read allow, Edit ask, Shell ask.
ISHLLMPermissionAction ISHLLMCategoryAction(ISHLLMToolCategory category);
void ISHLLMSetCategoryAction(ISHLLMToolCategory category, ISHLLMPermissionAction action);
// First match wins. Never saved until edited, so the defaults below can
// improve in a later release for everyone who has not changed them.
NSArray<NSDictionary<NSString *, id> *> *ISHLLMShellRules(void);
NSArray<NSDictionary<NSString *, id> *> *ISHLLMDefaultShellRules(void);
void ISHLLMSetShellRules(NSArray<NSDictionary<NSString *, id> *> *_Nullable rules); // nil restores the defaults
// Adds (or moves to the front) one rule, so it wins over anything older.
void ISHLLMAddShellRule(NSString *pattern, ISHLLMPermissionAction action);

// `*` matches any run of characters (including none), `?` exactly one;
// everything else literally, case-sensitively, against the whole text.
BOOL ISHLLMWildcardMatch(NSString *pattern, NSString *text);

// Splits a command line at the unquoted separators ; & && | || and newlines,
// honouring '...' "..." and backslash escapes, and trims each piece. Also
// reports whether any piece redirects output into a file (> or >>, but not
// >&N or 2>/dev/null-style descriptor moves) and whether the line contains a
// command substitution ($(...) or backquotes, outside single quotes) whose
// inner command no rule can see.
NSArray<NSString *> *ISHLLMSplitShellCommand(NSString *command,
                                             BOOL *_Nullable writesFileOut,
                                             BOOL *_Nullable substitutesOut);

// The answer for one shell command line. Each piece takes the action of the
// first rule it matches, or `fallback` when none does; a piece that writes a
// file or a line with a command substitution cannot be allowed by a rule
// alone (it gets `fallback`, as if no rule matched), since the rule was
// written for the command, not for what it could be turned into. The line's
// answer is the most severe piece. reasonOut names what decided it, phrased
// to follow "because of" in the confirmation prompt, or nil when it was just
// the category setting.
ISHLLMPermissionAction ISHLLMEvaluateShellCommand(NSString *command,
                                                  NSArray<NSDictionary<NSString *, id> *> *rules,
                                                  ISHLLMPermissionAction fallback,
                                                  NSString *_Nullable *_Nullable reasonOut);

// The answer for a file tool on `path` (absolute, normalised). An edit
// outside `workingDirectory` asks even when edits are allowed, as OpenCode
// does for paths outside the project; reads follow their setting anywhere.
// reasonOut as for ISHLLMEvaluateShellCommand.
ISHLLMPermissionAction ISHLLMEvaluatePathAccess(ISHLLMToolCategory category,
                                                NSString *path,
                                                NSString *workingDirectory,
                                                ISHLLMPermissionAction categoryAction,
                                                NSString *_Nullable *_Nullable reasonOut);

// YES if `path` is `directory` or inside it. Both absolute; compares whole
// components, so /root/a is not inside /root/ab.
BOOL ISHLLMPathIsInside(NSString *path, NSString *directory);

// A pattern the confirmation prompt can offer as "always allow": the
// command's first word and a wildcard ("git *"), or nil when the line has
// more than one piece, a substitution, or a redirect, where a pattern built
// from the first word would allow far more than the user was shown.
NSString *_Nullable ISHLLMSuggestedShellRule(NSString *command);

// The glob tool's pattern as a regular expression over a relative path,
// gitignore-style: * and ? stay within one component, ** crosses them ("**/"
// also matches no directory at all), {a,b} is either, [...] a set ([!...]
// negated). nil if the result does not compile. Here rather than with the
// tool so it can be tested on the host.
NSRegularExpression *_Nullable ISHLLMGlobRegex(NSString *glob);

NS_ASSUME_NONNULL_END
