//
//  LLMKeychain.m
//  iSH-AOK
//
//  See LLMKeychain.h.
//

#import "LLMKeychain.h"
#import <Security/Security.h>

NSString *const kISHLLMKeychainActiveAccount = @"active";

static NSString *const kISHLLMKeychainService = @"app.ish.iSH-AOK.llm-api-key";

NSString *ISHLLMKeychainDestinationAccount(NSString *destinationID) {
    return [@"destination:" stringByAppendingString:destinationID ?: @""];
}

// account -> value, with NSNull for "looked, none there".
static NSMutableDictionary<NSString *, id> *ISHLLMKeychainCache(void) {
    static NSMutableDictionary *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        cache = [NSMutableDictionary dictionary];
    });
    return cache;
}

static NSDictionary *ISHLLMKeychainQuery(NSString *account) {
    return @{
        (__bridge id) kSecClass: (__bridge id) kSecClassGenericPassword,
        (__bridge id) kSecAttrService: kISHLLMKeychainService,
        (__bridge id) kSecAttrAccount: account,
    };
}

NSString *ISHLLMKeychainRead(NSString *account) {
    NSMutableDictionary *cache = ISHLLMKeychainCache();
    @synchronized (cache) {
        id cached = cache[account];
        if (cached != nil)
            return cached == NSNull.null ? nil : cached;
    }
    NSMutableDictionary *query = [ISHLLMKeychainQuery(account) mutableCopy];
    query[(__bridge id) kSecReturnData] = @YES;
    query[(__bridge id) kSecMatchLimit] = (__bridge id) kSecMatchLimitOne;
    CFTypeRef result = NULL;
    OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef) query, &result);
    NSString *value = nil;
    if (status == errSecSuccess && result != NULL)
        value = [[NSString alloc] initWithData:(__bridge_transfer NSData *) result encoding:NSUTF8StringEncoding];
    // A locked Keychain (before first unlock) is not "no key": do not cache
    // that answer, or the key would stay missing until the app restarts.
    if (status == errSecSuccess || status == errSecItemNotFound) {
        @synchronized (cache) {
            cache[account] = value ?: (id) NSNull.null;
        }
    }
    return value.length > 0 ? value : nil;
}

BOOL ISHLLMKeychainWrite(NSString *account, NSString *value) {
    NSDictionary *query = ISHLLMKeychainQuery(account);
    OSStatus status;
    if (value.length == 0) {
        status = SecItemDelete((__bridge CFDictionaryRef) query);
        if (status == errSecItemNotFound)
            status = errSecSuccess;
    } else {
        NSData *data = [value dataUsingEncoding:NSUTF8StringEncoding];
        NSDictionary *attributes = @{
            (__bridge id) kSecValueData: data,
            // The chat keeps working when the app runs in the background
            // (a long tool loop), which is after the first unlock but not
            // necessarily while unlocked. Never synced or backed up to
            // another device.
            (__bridge id) kSecAttrAccessible: (__bridge id) kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
        };
        status = SecItemUpdate((__bridge CFDictionaryRef) query, (__bridge CFDictionaryRef) attributes);
        if (status == errSecItemNotFound) {
            NSMutableDictionary *add = [query mutableCopy];
            [add addEntriesFromDictionary:attributes];
            status = SecItemAdd((__bridge CFDictionaryRef) add, NULL);
        }
    }
    if (status != errSecSuccess) {
        NSLog(@"LLM Keychain: writing %@ failed (%d)", account, (int) status);
        return NO;
    }
    NSMutableDictionary *cache = ISHLLMKeychainCache();
    @synchronized (cache) {
        cache[account] = value.length > 0 ? [value copy] : (id) NSNull.null;
    }
    return YES;
}
