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

bool amd64_jit_preference_get(void);
void amd64_jit_preference_set(bool enabled);
