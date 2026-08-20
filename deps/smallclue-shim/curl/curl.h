// Just enough of libcurl's easy API for deps/smallclue/src/core.c, implemented
// on NSURLSession.
//
// WHY THIS EXISTS. curl, wget and `md <url>` are the applets SmallCLUE gates on
// PSCAL_HAS_LIBCURL, and AOK could not define it: the iOS SDK ships no libcurl
// at all -- no curl/curl.h, no libcurl.tbd -- so the one build that matters had
// nothing to link. macOS has both, but a shim that exists only on the desktop
// is a shim that is never tested on the device it is for.
//
// NSURLSession is on both, and is the platform's supported HTTP client: TLS,
// HTTP/2, proxies, redirects and content decoding are its problem rather than
// ours. deps/smallclue-shim/openssl/evp.h took the same road to CommonCrypto
// for the same reason, and nothing new is linked either time.
//
// WHAT IS GIVEN UP, said plainly. This is host networking. Every other entry in
// the native shim routes a call INTO the guest -- that is what kernel/
// native_libc.h is for -- and this one cannot, because NSURLSession is a
// prebuilt framework whose sockets are opened by dyld-resolved code the shim
// has no way to redirect. Concretely, a fetch through here:
//
//   - resolves names through the DEVICE's resolver, so the guest's
//     /etc/resolv.conf and /etc/hosts do not apply;
//   - does not appear in the guest's /proc/net/tcp, and holds no guest fd;
//   - uses the device's proxy and VPN configuration, not the guest's.
//
// For a request to a public host over the device's own network -- which is
// what curl, wget and `md <url>` do -- none of that is observable. For anything
// resolved by a guest-local name it is, and the emulated curl in the distro's
// own package is the tool that gets it right, because it runs on guest sockets
// like everything else.
//
// This is NOT the zlib situation (deps/smallclue-shim/zlib.h), where the host
// library silently read and wrote the wrong FILESYSTEM. The bytes here never
// touch the host's disk: they arrive in memory and go out through the caller's
// write callback, which is SmallCLUE's own code and therefore redirected. What
// is host-side is the transport, deliberately, and it is the reason
// tools/check-native-libc.py does not scan this archive -- see the note there.
//
// SCOPE. The seven functions and twenty-five options core.c uses. Not a libcurl
// compatibility layer, and it must not grow into one: a consumer wanting FTP,
// cookies, multipart or the multi interface should get a real library rather
// than an ever-widening imitation of one. Anything missing is a compile error
// naming the symbol, which is the correct outcome.

#ifndef AOK_SMALLCLUE_SHIM_CURL_H
#define AOK_SMALLCLUE_SHIM_CURL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// core.c tests this to choose CURLOPT_PROTOCOLS_STR over the numeric
// CURLOPT_PROTOCOLS. Both are accepted below; the value picks the spelling
// that has not been deprecated.
#define LIBCURL_VERSION_NUM 0x080000

typedef void CURL;

typedef enum {
    CURLE_OK = 0,
    CURLE_UNSUPPORTED_PROTOCOL = 1,
    CURLE_FAILED_INIT = 2,
    CURLE_URL_MALFORMAT = 3,
    CURLE_COULDNT_RESOLVE_HOST = 6,
    CURLE_COULDNT_CONNECT = 7,
    CURLE_HTTP_RETURNED_ERROR = 22,
    CURLE_WRITE_ERROR = 23,
    CURLE_OUT_OF_MEMORY = 27,
    CURLE_OPERATION_TIMEDOUT = 28,
    CURLE_SSL_CONNECT_ERROR = 35,
    CURLE_ABORTED_BY_CALLBACK = 42,
    CURLE_TOO_MANY_REDIRECTS = 47,
    CURLE_UNKNOWN_OPTION = 48,
    CURLE_RECV_ERROR = 56,
    CURLE_PEER_FAILED_VERIFICATION = 60,
} CURLcode;

// Real libcurl numbers these by type so its varargs unpacking knows what to
// pull. Nothing here depends on the encoding -- aok_curl_setopt switches on the
// name -- but the values are kept so that a stray printf of an option is
// recognisable against curl's own headers.
typedef enum {
    CURLOPT_WRITEDATA = 10001,
    CURLOPT_URL = 10002,
    CURLOPT_USERPWD = 10005,
    CURLOPT_POSTFIELDS = 10015,
    CURLOPT_USERAGENT = 10018,
    CURLOPT_LOW_SPEED_LIMIT = 19,
    CURLOPT_LOW_SPEED_TIME = 20,
    CURLOPT_WRITEFUNCTION = 20011,
    CURLOPT_TIMEOUT = 13,
    CURLOPT_POSTFIELDSIZE = 60,
    CURLOPT_HTTPHEADER = 10023,
    CURLOPT_FOLLOWLOCATION = 52,
    CURLOPT_MAXREDIRS = 68,
    CURLOPT_CUSTOMREQUEST = 10036,
    CURLOPT_CONNECTTIMEOUT = 78,
    CURLOPT_HTTPAUTH = 107,
    CURLOPT_SSL_VERIFYPEER = 64,
    CURLOPT_SSL_VERIFYHOST = 81,
    CURLOPT_ACCEPT_ENCODING = 10102,
    CURLOPT_NOSIGNAL = 99,
    CURLOPT_PROTOCOLS = 181,
    CURLOPT_REDIR_PROTOCOLS = 182,
    CURLOPT_TCP_KEEPALIVE = 213,
    CURLOPT_PROTOCOLS_STR = 10318,
    CURLOPT_REDIR_PROTOCOLS_STR = 10319,
} CURLoption;

#define CURLPROTO_HTTP  (1 << 0)
#define CURLPROTO_HTTPS (1 << 1)

#define CURLAUTH_BASIC (((unsigned long) 1) << 0)
#define CURLAUTH_ANY   (~((unsigned long) 0))

struct curl_slist {
    char *data;
    struct curl_slist *next;
};

typedef size_t (*curl_write_callback)(char *buffer, size_t size, size_t nitems,
                                      void *outstream);

CURL *curl_easy_init(void);
CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *handle);
void curl_easy_cleanup(CURL *handle);
const char *curl_easy_strerror(CURLcode code);

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *string);
void curl_slist_free_all(struct curl_slist *list);

#ifdef __cplusplus
}
#endif

#endif
