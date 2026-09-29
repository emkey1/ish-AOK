//
//  LLMKeychain.h
//  iSH-AOK
//
//  The LLM Chat's API keys, in the Keychain. They used to be plain
//  NSUserDefaults values, and every defaults key is readable by any guest
//  process as /proc/ish/defaults/<name> -- so a model with run_shell, or any
//  package, could read the user's keys. Moved 2026-09-29; the old values are
//  migrated on first read and removed from the defaults.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// The key the chat currently uses (UserPreferences.llmAPIKey's storage).
extern NSString *const kISHLLMKeychainActiveAccount;

// A saved destination's key.
NSString *ISHLLMKeychainDestinationAccount(NSString *destinationID);

// nil when there is none. Cached after the first read, so the request paths
// that ask for the key on every message do not pay a Keychain round trip.
NSString *_Nullable ISHLLMKeychainRead(NSString *account);
// An empty or nil value deletes the item. NO if the Keychain refused, in
// which case the caller keeps whatever it had.
BOOL ISHLLMKeychainWrite(NSString *account, NSString *_Nullable value);

NS_ASSUME_NONNULL_END
