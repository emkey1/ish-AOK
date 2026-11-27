//
//  LLMChatSkills.h
//  AOK-Workstation
//
//  Agent Skills (agentskills.io) for the LLM Chat: a skill is a folder with a
//  SKILL.md in it, YAML frontmatter naming it and saying when to use it, and
//  instructions below that. The chat lists each skill's name, description and
//  location in its standing instructions; the model reads a SKILL.md with
//  read_file when a task matches, and runs what it bundles with run_shell,
//  under the usual permissions. Only the list costs anything up front.
//
//  This file is the part with no guest access: where to look, the
//  frontmatter, which skill wins a name, and the list the model sees.
//  Foundation only, so it is tested on its own (tests/unit/llm_skills_test.m).
//  Finding the files is ISHLLMDiscoverSkills in LLMChatTools.m.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// The folder every root shares, for skills meant for all of them. It is the
// Files app's Workstation > Persist > skills.
extern NSString *const kISHLLMSharedSkillsDirectory;

// Where a skill was found. Earlier scopes win a name.
typedef NS_ENUM(NSInteger, ISHLLMSkillScope) {
    ISHLLMSkillScopeProject, // .agents/skills or .claude/skills at or above the working directory
    ISHLLMSkillScopeUser,    // the same two under the home directory
    ISHLLMSkillScopeShared,  // kISHLLMSharedSkillsDirectory
};

@interface ISHLLMSkill : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *skillDescription;
@property (nonatomic, copy) NSString *location; // the SKILL.md, absolute
@property (nonatomic) ISHLLMSkillScope scope;
// Frontmatter `disable-model-invocation: true` (Claude Code's): the author
// wants it used only when asked for by name, which this chat has no way to
// do, so it is not listed.
@property (nonatomic) BOOL disableModelInvocation;
// Nil when the skill is offered to the model; otherwise why not, for /skills.
@property (nonatomic, copy, nullable) NSString *notOfferedReason;
@end

// The skills folders to look in, nearest first: .agents/skills (the
// cross-client convention) then .claude/skills (where many skills are
// installed) in the working directory and each folder above it, stopping
// before the home directory and "/"; the same two in the home directory; then
// the shared one. Each entry is @{@"path": NSString, @"scope": NSNumber}.
NSArray<NSDictionary<NSString *, id> *> *ISHLLMSkillSearchDirectories(NSString *_Nullable workingDirectory,
                                                                       NSString *_Nullable homeDirectory);

// Reads a SKILL.md's frontmatter the way other clients do, which is more
// leniently than YAML: a value may hold an unquoted colon, run on over
// indented lines, be quoted, or be a > or | block. A missing name takes the
// folder's; a file with no frontmatter or no description comes back with
// notOfferedReason set. Never nil.
ISHLLMSkill *ISHLLMParseSkill(NSString *text, NSString *location, NSString *folderName, ISHLLMSkillScope scope);

// Decides which of the skills found are offered, in the order found: the
// first of a name wins, a skill that asked not to be invoked by the model is
// left out, and the list stops at budgetCharacters. Sets notOfferedReason on
// the rest and returns the ones offered.
NSArray<ISHLLMSkill *> *ISHLLMResolveSkills(NSArray<ISHLLMSkill *> *found, NSUInteger budgetCharacters);

// The section of the tool note that lists the offered skills, or nil when
// there are none (an empty list would only confuse the model).
NSString *_Nullable ISHLLMSkillCatalog(NSArray<ISHLLMSkill *> *offered);

// Settings > LLM Client > Skills. On unless turned off.
BOOL ISHLLMSkillsEnabled(void);
void ISHLLMSetSkillsEnabled(BOOL enabled);

// How much of every request the list may take, in characters: about 3,000
// tokens, a few dozen typical skills.
extern const NSUInteger kISHLLMSkillCatalogBudget;

NS_ASSUME_NONNULL_END
