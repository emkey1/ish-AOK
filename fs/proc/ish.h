#include <stddef.h>
#include <stdbool.h>

struct user_default_key {
    char *name;
    char *underlying_name;
};

extern char **(*get_all_defaults_keys)(void);
extern char *(*get_friendly_name)(const char *name);
extern char *(*get_underlying_name)(const char *name);
extern bool (*get_user_default)(const char *name, char **buffer, size_t *size);
extern bool (*set_user_default)(const char *name, char *buffer, size_t size);
extern bool (*remove_user_default)(const char *name);
extern char *(*get_documents_directory)(void);

// The root-filesystem manager lives in the iOS app (app/Roots.m), so these two
// are installed there and stay NULL in the command-line build, which has no
// roots to manage: it boots whatever -f named and that is the whole story.
// Both /proc/ish/roots handlers check before calling.
//
// ish_roots_status returns the whole /proc/ish/roots body, malloc'd for the
// caller to free. ish_roots_command takes one complete command and returns 0
// or a negative errno; it never blocks, because anything slow (a download, an
// extraction) is a job whose progress shows up in the status.
extern char *(*ish_roots_status)(void);
extern int (*ish_roots_command)(const char *command);

// The Workspace bridge, same shape and the same NULL-in-the-CLI-build rule.
//
// A guest cannot otherwise tell it is running under Workspace, and has no way
// to ask the app to put anything on screen -- GuestFileBridge goes the other
// way, the app reading guest files. /proc/ish/workspace is both halves: read
// it to find out whether there is a Workspace at all and what tools it has,
// write to it to ask for one.
//
// ish_workspace_status returns the whole body, malloc'd for the caller to
// free. ish_workspace_open takes one complete request and returns 0 or a
// negative errno. It must not block: it is called from a guest write(2), and
// the app does the actual presenting on its own queue.
extern char *(*ish_workspace_status)(void);
extern int (*ish_workspace_open)(const char *request);

bool amd64_jit_preference_get(void);
void amd64_jit_preference_set(bool enabled);
