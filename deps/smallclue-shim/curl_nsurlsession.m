// Implementation of deps/smallclue-shim/curl/curl.h -- read that first, in
// particular the paragraph on what host networking gives up.
//
// THREADS, which is the part that is easy to get wrong here. A native program
// runs on a guest task's own thread (kernel/native.h), and the shim's
// redirected libc works only on such a thread: it needs the current task to
// reach the guest's fd table. NSURLSession's delegate callbacks arrive on a
// queue of its own, on threads that have no task at all.
//
// So the delegate below touches nothing but plain memory -- it appends bytes to
// a buffer and signals a condition variable. The caller's write callback, which
// is SmallCLUE's code and does write to a guest FILE*, is invoked from
// curl_easy_perform on the ORIGINAL thread. Calling it from the delegate would
// compile, run, and write the response into whatever the host made of a guest
// fd number.
//
// The same split is why the wait is a timed one. native_checkpoint() is how a
// native program notices a signal, so ^C during a download only works if the
// waiting thread comes up for air; it sleeps in slices and checkpoints between
// them, exactly as nlibc_sleep_us does.

#import <Foundation/Foundation.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "kernel/native.h"

// How long the caller's thread parks before checking for a signal. Same
// reasoning as SC_SLEEP_SLICE_US in kernel/native_libc.c: short enough that a
// keystroke lands promptly, long enough to cost nothing.
#define AOK_CURL_WAIT_SLICE_NS (50 * 1000 * 1000)

// Above this many buffered bytes the transfer is suspended until the caller
// drains it. Without backpressure a fast link and a slow consumer would hold
// the whole response in memory, which for `wget` of a rootfs is the difference
// between working and being killed.
#define AOK_CURL_HIGH_WATER (4u * 1024u * 1024u)

struct aok_curl_handle {
    char *url;
    char *method;
    char *useragent;
    char *userpwd;
    char *accept_encoding;
    struct curl_slist *headers;   // borrowed from the caller, never freed here
    void *postfields;
    size_t postfieldsize;
    size_t postfields_cap;   // what was actually copied; postfieldsize cannot exceed it
    long followlocation;
    long maxredirs;
    long connecttimeout;
    long timeout;
    long low_speed_limit;
    long low_speed_time;
    long ssl_verifypeer;
    curl_write_callback writefn;
    void *writedata;
    // -I, -i, -D, -f.
    long nobody;
    long header;            // headers into the WRITE stream too (curl -i)
    long failonerror;
    curl_write_callback headerfn;
    void *headerdata;
    // The last transfer, for curl_easy_getinfo.
    long info_code;
    long info_redirects;
    long info_http_version;
    double info_total_time;
    long long info_size;
    char *info_url;
    char *info_content_type;
};

// Shared between the calling thread and NSURLSession's delegate queue. Plain C
// on purpose: see the header comment about what a delegate thread may touch.
struct aok_curl_xfer {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned char *buf;
    size_t len;
    size_t cap;
    int done;
    int suspended;
    CURLcode result;
    long redirects;
    // Copies rather than a pointer to the handle. curl_easy_cleanup() may run
    // the instant curl_easy_perform returns, and a delegate callback can still
    // be in flight on NSURLSession's queue at that moment -- core.c does
    // exactly that sequence on the error path.
    long followlocation;
    long maxredirs;
    long ssl_verifypeer;
    // What the framework actually said, for curl_easy_strerror to hand back.
    // A CURLcode is seventeen buckets; an NSError names the problem.
    char detail[256];
    // The response, once it arrives (didReceiveResponse), for the header
    // callback and curl_easy_getinfo. All malloc'd C, freed with x.
    long failonerror;
    int have_response;
    long code;
    char *headers;          // "Name: value\r\n"..., without the status line
    char *url;
    char *content_type;
    char protocol[16];      // "h2", "http/1.1", ... from the task metrics
};

// ------------------------------------------------------------------ helpers

static char *aok_strdup_or_null(const char *s) {
    return s ? strdup(s) : NULL;
}

static void aok_replace(char **slot, const char *value) {
    free(*slot);
    *slot = aok_strdup_or_null(value);
}

static BOOL aok_scheme_allowed(NSURL *url) {
    NSString *scheme = url.scheme.lowercaseString;
    return [scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"];
}

static int aok_xfer_append(struct aok_curl_xfer *x, const void *bytes, size_t n) {
    if (x->len + n > x->cap) {
        size_t want = x->cap ? x->cap : 65536;
        while (want < x->len + n)
            want *= 2;
        unsigned char *grown = realloc(x->buf, want);
        if (!grown)
            return -1;
        x->buf = grown;
        x->cap = want;
    }
    memcpy(x->buf + x->len, bytes, n);
    x->len += n;
    return 0;
}

// ------------------------------------------------------------------ delegate

// The delegate OWNS the transfer state, which is why that state is on the heap
// and not in curl_easy_perform's frame where it started.
//
// [NSURLSession invalidateAndCancel] is asynchronous: it returns at once and
// the queue may still deliver didCompleteWithError afterwards. A stack struct
// would be gone by then. Tying the lifetime to the delegate instead makes
// every ordering safe -- perform holds a strong reference for its whole body,
// the session holds one until invalidation finishes, and whichever is last
// releases it.
@interface AOKCurlDelegate : NSObject <NSURLSessionDataDelegate>
@property (nonatomic, assign) struct aok_curl_xfer *xfer;
@end

@implementation AOKCurlDelegate

- (void)dealloc {
    struct aok_curl_xfer *x = self.xfer;
    if (!x)
        return;
    self.xfer = NULL;
    free(x->buf);
    free(x->headers);
    free(x->url);
    free(x->content_type);
    pthread_cond_destroy(&x->cv);
    pthread_mutex_destroy(&x->mu);
    free(x);
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
didReceiveResponse:(NSURLResponse *)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    struct aok_curl_xfer *x = self.xfer;
    NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class]
        ? (NSHTTPURLResponse *) response : nil;
    NSMutableString *block = [NSMutableString string];
    for (NSString *name in http.allHeaderFields) {
        [block appendFormat:@"%@: %@\r\n", name, http.allHeaderFields[name]];
    }
    const char *headers = block.UTF8String;
    const char *url = response.URL.absoluteString.UTF8String;
    const char *type = http ? [http valueForHTTPHeaderField:@"Content-Type"].UTF8String : NULL;
    pthread_mutex_lock(&x->mu);
    x->have_response = 1;
    x->code = http ? (long) http.statusCode : 0;
    x->headers = headers ? strdup(headers) : NULL;
    x->url = url ? strdup(url) : NULL;
    x->content_type = type ? strdup(type) : NULL;
    BOOL refuse = x->failonerror && x->code >= 400;
    if (refuse) {
        // curl -f: an error status is a failed transfer, and its body is not
        // output. Cancelled here rather than drained.
        x->result = CURLE_HTTP_RETURNED_ERROR;
        snprintf(x->detail, sizeof(x->detail),
                 "The requested URL returned error: %ld", x->code);
    }
    pthread_cond_signal(&x->cv);
    pthread_mutex_unlock(&x->mu);
    completionHandler(refuse ? NSURLSessionResponseCancel : NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session
              task:(NSURLSessionTask *)task
didFinishCollectingMetrics:(NSURLSessionTaskMetrics *)metrics {
    // The protocol a response came over, which NSHTTPURLResponse does not
    // say: "h2" or "http/1.1". Collected before didCompleteWithError, so a
    // HEAD request (curl -I) always has it; a body may begin before it.
    NSString *proto = metrics.transactionMetrics.lastObject.networkProtocolName;
    struct aok_curl_xfer *x = self.xfer;
    pthread_mutex_lock(&x->mu);
    if (proto.UTF8String)
        snprintf(x->protocol, sizeof(x->protocol), "%s", proto.UTF8String);
    pthread_mutex_unlock(&x->mu);
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveData:(NSData *)data {
    struct aok_curl_xfer *x = self.xfer;
    __block int failed = 0;
    pthread_mutex_lock(&x->mu);
    [data enumerateByteRangesUsingBlock:^(const void *bytes, NSRange range, BOOL *stop) {
        if (aok_xfer_append(x, bytes, range.length) < 0) {
            failed = 1;
            *stop = YES;
        }
    }];
    if (failed) {
        x->result = CURLE_OUT_OF_MEMORY;
        x->done = 1;
    } else if (x->len >= AOK_CURL_HIGH_WATER && !x->suspended) {
        x->suspended = 1;
        [dataTask suspend];
    }
    pthread_cond_signal(&x->cv);
    pthread_mutex_unlock(&x->mu);
}

- (void)URLSession:(NSURLSession *)session
              task:(NSURLSessionTask *)task
didCompleteWithError:(NSError *)error {
    struct aok_curl_xfer *x = self.xfer;
    pthread_mutex_lock(&x->mu);
    if (error && error.code != NSURLErrorCancelled) {
        // Cancellation is ours -- a refused redirect or a failed write -- and
        // its description would replace the reason we already recorded.
        const char *text = error.localizedDescription.UTF8String;
        if (text)
            snprintf(x->detail, sizeof(x->detail), "%s", text);
    }
    if (error && x->result == CURLE_OK) {
        switch (error.code) {
            case NSURLErrorAppTransportSecurityRequiresSecureConnection:
                // Not a network failure at all: the request never left. The
                // app's Info.plist decides this, and a plain-http URL is the
                // way to meet it.
                x->result = CURLE_UNSUPPORTED_PROTOCOL; break;
            case NSURLErrorCannotFindHost:
            case NSURLErrorDNSLookupFailed:
                x->result = CURLE_COULDNT_RESOLVE_HOST; break;
            case NSURLErrorCannotConnectToHost:
            case NSURLErrorNetworkConnectionLost:
            case NSURLErrorNotConnectedToInternet:
                x->result = CURLE_COULDNT_CONNECT; break;
            case NSURLErrorTimedOut:
                x->result = CURLE_OPERATION_TIMEDOUT; break;
            case NSURLErrorSecureConnectionFailed:
                x->result = CURLE_SSL_CONNECT_ERROR; break;
            case NSURLErrorServerCertificateHasBadDate:
            case NSURLErrorServerCertificateUntrusted:
            case NSURLErrorServerCertificateHasUnknownRoot:
            case NSURLErrorServerCertificateNotYetValid:
                x->result = CURLE_PEER_FAILED_VERIFICATION; break;
            case NSURLErrorHTTPTooManyRedirects:
                x->result = CURLE_TOO_MANY_REDIRECTS; break;
            case NSURLErrorUnsupportedURL:
            case NSURLErrorBadURL:
                x->result = CURLE_URL_MALFORMAT; break;
            case NSURLErrorCancelled:
                // Our own doing -- a refused redirect or a failed write already
                // set the code it should report.
                break;
            default:
                x->result = CURLE_RECV_ERROR; break;
        }
    }
    x->done = 1;
    pthread_cond_signal(&x->cv);
    pthread_mutex_unlock(&x->mu);
}

- (void)URLSession:(NSURLSession *)session
              task:(NSURLSessionTask *)task
willPerformHTTPRedirection:(NSHTTPURLResponse *)response
        newRequest:(NSURLRequest *)request
 completionHandler:(void (^)(NSURLRequest *))completionHandler {
    struct aok_curl_xfer *x = self.xfer;
    pthread_mutex_lock(&x->mu);
    long followed = ++x->redirects;
    long maxredirs = x->maxredirs;
    long follow = x->followlocation;
    if (!follow) {
        pthread_mutex_unlock(&x->mu);
        // Real curl with FOLLOWLOCATION off treats the 3xx as the answer and
        // hands its body to the write callback, which is what nil does here.
        completionHandler(nil);
        return;
    }
    if (maxredirs >= 0 && followed > maxredirs) {
        x->result = CURLE_TOO_MANY_REDIRECTS;
        pthread_mutex_unlock(&x->mu);
        [task cancel];
        completionHandler(nil);
        return;
    }
    if (!aok_scheme_allowed(request.URL)) {
        // CURLOPT_REDIR_PROTOCOLS is set to http,https by core.c precisely to
        // stop a redirect to file:// or similar. NSURLSession would not follow
        // one to file://, but saying no here is the guarantee the caller asked
        // for rather than a property of the framework.
        x->result = CURLE_UNSUPPORTED_PROTOCOL;
        pthread_mutex_unlock(&x->mu);
        [task cancel];
        completionHandler(nil);
        return;
    }
    pthread_mutex_unlock(&x->mu);
    completionHandler(request);
}

- (void)URLSession:(NSURLSession *)session
didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge
 completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))completionHandler {
    struct aok_curl_xfer *x = self.xfer;
    if (x->ssl_verifypeer == 0 &&
        [challenge.protectionSpace.authenticationMethod
            isEqualToString:NSURLAuthenticationMethodServerTrust]) {
        SecTrustRef trust = challenge.protectionSpace.serverTrust;
        if (trust) {
            completionHandler(NSURLSessionAuthChallengeUseCredential,
                              [NSURLCredential credentialForTrust:trust]);
            return;
        }
    }
    completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
}

@end

// -------------------------------------------------------------- the easy API

CURL *curl_easy_init(void) {
    struct aok_curl_handle *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->followlocation = 0;
    h->maxredirs = -1;
    h->ssl_verifypeer = 1;
    return h;
}

CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...) {
    struct aok_curl_handle *h = handle;
    if (!h)
        return CURLE_FAILED_INIT;

    va_list ap;
    va_start(ap, option);
    CURLcode rc = CURLE_OK;
    switch (option) {
        case CURLOPT_URL:             aok_replace(&h->url, va_arg(ap, const char *)); break;
        case CURLOPT_CUSTOMREQUEST:   aok_replace(&h->method, va_arg(ap, const char *)); break;
        case CURLOPT_USERAGENT:       aok_replace(&h->useragent, va_arg(ap, const char *)); break;
        case CURLOPT_USERPWD:         aok_replace(&h->userpwd, va_arg(ap, const char *)); break;
        case CURLOPT_ACCEPT_ENCODING: aok_replace(&h->accept_encoding, va_arg(ap, const char *)); break;
        case CURLOPT_HTTPHEADER:      h->headers = va_arg(ap, struct curl_slist *); break;
        case CURLOPT_WRITEFUNCTION:   h->writefn = va_arg(ap, curl_write_callback); break;
        case CURLOPT_WRITEDATA:       h->writedata = va_arg(ap, void *); break;
        case CURLOPT_FOLLOWLOCATION:  h->followlocation = va_arg(ap, long); break;
        case CURLOPT_MAXREDIRS:       h->maxredirs = va_arg(ap, long); break;
        case CURLOPT_CONNECTTIMEOUT:  h->connecttimeout = va_arg(ap, long); break;
        case CURLOPT_TIMEOUT:         h->timeout = va_arg(ap, long); break;
        case CURLOPT_LOW_SPEED_LIMIT: h->low_speed_limit = va_arg(ap, long); break;
        case CURLOPT_LOW_SPEED_TIME:  h->low_speed_time = va_arg(ap, long); break;
        case CURLOPT_SSL_VERIFYPEER:  h->ssl_verifypeer = va_arg(ap, long); break;
        case CURLOPT_NOBODY:          h->nobody = va_arg(ap, long); break;
        case CURLOPT_HEADER:          h->header = va_arg(ap, long); break;
        case CURLOPT_FAILONERROR:     h->failonerror = va_arg(ap, long); break;
        case CURLOPT_HEADERFUNCTION:  h->headerfn = va_arg(ap, curl_write_callback); break;
        case CURLOPT_HEADERDATA:      h->headerdata = va_arg(ap, void *); break;
        case CURLOPT_POSTFIELDS: {
            const void *data = va_arg(ap, const void *);
            free(h->postfields);
            h->postfields = NULL;
            h->postfields_cap = 0;
            if (data) {
                // Real libcurl does not copy here (that is COPYPOSTFIELDS) and
                // reads the length from POSTFIELDSIZE, which may be set either
                // side of this call. Copying up to the NUL is the safe reading
                // of a pointer with no length yet -- and it is why
                // postfields_cap exists: a POSTFIELDSIZE arriving afterwards
                // says how much of a buffer we no longer have to read.
                size_t n = strlen((const char *) data);
                h->postfields = malloc(n + 1);
                if (h->postfields) {
                    memcpy(h->postfields, data, n + 1);
                    h->postfields_cap = n;
                    if (h->postfieldsize == 0)
                        h->postfieldsize = n;
                }
            } else {
                h->postfieldsize = 0;
            }
            break;
        }
        case CURLOPT_POSTFIELDSIZE: {
            // curl spells "work it out yourself" as -1.
            long n = va_arg(ap, long);
            if (n >= 0)
                h->postfieldsize = (size_t) n;
            break;
        }
        // Accepted and deliberately inert. NSURLSession keeps connections alive
        // and never raises a signal, the protocol restrictions are enforced in
        // the redirect delegate and before the request is built, and
        // SSL_VERIFYHOST has no separate meaning once VERIFYPEER decides
        // whether the trust evaluation is honoured at all.
        case CURLOPT_TCP_KEEPALIVE:
        case CURLOPT_NOSIGNAL:
        case CURLOPT_SSL_VERIFYHOST:
        case CURLOPT_HTTPAUTH:
        case CURLOPT_PROTOCOLS:
        case CURLOPT_REDIR_PROTOCOLS:
            (void) va_arg(ap, long);
            break;
        case CURLOPT_PROTOCOLS_STR:
        case CURLOPT_REDIR_PROTOCOLS_STR:
            (void) va_arg(ap, const char *);
            break;
        default:
            rc = CURLE_UNKNOWN_OPTION;
            break;
    }
    va_end(ap);
    return rc;
}

void curl_easy_cleanup(CURL *handle) {
    struct aok_curl_handle *h = handle;
    if (!h)
        return;
    free(h->url);
    free(h->method);
    free(h->useragent);
    free(h->userpwd);
    free(h->accept_encoding);
    free(h->postfields);
    free(h->info_url);
    free(h->info_content_type);
    free(h);
}

CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...) {
    struct aok_curl_handle *h = handle;
    if (!h)
        return CURLE_FAILED_INIT;
    va_list ap;
    va_start(ap, info);
    CURLcode rc = CURLE_OK;
    switch (info) {
        case CURLINFO_RESPONSE_CODE:   *va_arg(ap, long *) = h->info_code; break;
        case CURLINFO_REDIRECT_COUNT:  *va_arg(ap, long *) = h->info_redirects; break;
        case CURLINFO_HTTP_VERSION:    *va_arg(ap, long *) = h->info_http_version; break;
        case CURLINFO_TOTAL_TIME:      *va_arg(ap, double *) = h->info_total_time; break;
        case CURLINFO_SIZE_DOWNLOAD_T: *va_arg(ap, curl_off_t *) = h->info_size; break;
        case CURLINFO_EFFECTIVE_URL:   *va_arg(ap, char **) = h->info_url ? h->info_url : h->url; break;
        case CURLINFO_CONTENT_TYPE:    *va_arg(ap, char **) = h->info_content_type; break;
        default:                       rc = CURLE_UNKNOWN_OPTION; break;
    }
    va_end(ap);
    return rc;
}

// The status line curl prints for a response: "HTTP/2 200 " for h2 and h3 --
// with the space, and no reason -- and "HTTP/1.1 200 OK" for HTTP/1.1.
static const char *aok_curl_reason(long code) {
    switch (code) {
        case 200: return "OK"; case 201: return "Created"; case 202: return "Accepted";
        case 204: return "No Content"; case 206: return "Partial Content";
        case 301: return "Moved Permanently"; case 302: return "Found";
        case 303: return "See Other"; case 304: return "Not Modified";
        case 307: return "Temporary Redirect"; case 308: return "Permanent Redirect";
        case 400: return "Bad Request"; case 401: return "Unauthorized";
        case 403: return "Forbidden"; case 404: return "Not Found";
        case 405: return "Method Not Allowed"; case 409: return "Conflict";
        case 429: return "Too Many Requests"; case 500: return "Internal Server Error";
        case 502: return "Bad Gateway"; case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
    }
    return "";
}

static long aok_curl_http_version(const char *protocol) {
    if (strcmp(protocol, "h2") == 0) return 3;
    if (strcmp(protocol, "h3") == 0) return 30;
    if (strcmp(protocol, "http/1.0") == 0) return 1;
    return 2;
}

// The response's header block, status line first, as curl hands it to its
// header callback (and with -i to the output). NULL until there is one.
static char *aok_curl_header_block(struct aok_curl_xfer *x) {
    long version = aok_curl_http_version(x->protocol);
    const char *ver = version == 3 ? "2" : version == 30 ? "3" : version == 1 ? "1.0" : "1.1";
    const char *reason = version >= 3 ? "" : aok_curl_reason(x->code);
    size_t n = strlen(x->headers ? x->headers : "") + 64;
    char *block = malloc(n);
    if (block)
        snprintf(block, n, "HTTP/%s %ld %s\r\n%s\r\n", ver, x->code,
                 reason, x->headers ? x->headers : "");
    return block;
}

// Set by curl_easy_perform from the transfer it just finished, and preferred
// by curl_easy_strerror over the generic table below. core.c calls the two in
// that order, which is what makes a thread-local the right scope for it.
static _Thread_local char aok_curl_last_detail[256];

const char *curl_easy_strerror(CURLcode code) {
    if (code != CURLE_OK && aok_curl_last_detail[0] != '\0')
        return aok_curl_last_detail;
    switch (code) {
        case CURLE_OK:                       return "No error";
        case CURLE_UNSUPPORTED_PROTOCOL:     return "Unsupported protocol";
        case CURLE_FAILED_INIT:              return "Failed initialization";
        case CURLE_URL_MALFORMAT:            return "URL using bad/illegal format or missing URL";
        case CURLE_COULDNT_RESOLVE_HOST:     return "Could not resolve host";
        case CURLE_COULDNT_CONNECT:          return "Failed to connect to host";
        case CURLE_HTTP_RETURNED_ERROR:      return "HTTP response code said error";
        case CURLE_WRITE_ERROR:              return "Failed writing received data";
        case CURLE_OUT_OF_MEMORY:            return "Out of memory";
        case CURLE_OPERATION_TIMEDOUT:       return "Timeout was reached";
        case CURLE_SSL_CONNECT_ERROR:        return "SSL connect error";
        case CURLE_ABORTED_BY_CALLBACK:      return "Operation was aborted by an application callback";
        case CURLE_TOO_MANY_REDIRECTS:       return "Number of redirects hit maximum amount";
        case CURLE_UNKNOWN_OPTION:           return "An unknown option was passed in to libcurl";
        case CURLE_RECV_ERROR:               return "Failure when receiving data from the peer";
        case CURLE_PEER_FAILED_VERIFICATION: return "SSL peer certificate or SSH remote key was not OK";
    }
    return "Unknown error";
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *string) {
    if (!string)
        return list;
    struct curl_slist *node = calloc(1, sizeof(*node));
    if (!node)
        return list;
    node->data = strdup(string);
    if (!node->data) {
        free(node);
        return list;
    }
    if (!list)
        return node;
    struct curl_slist *tail = list;
    while (tail->next)
        tail = tail->next;
    tail->next = node;
    return list;
}

void curl_slist_free_all(struct curl_slist *list) {
    while (list) {
        struct curl_slist *next = list->next;
        free(list->data);
        free(list);
        list = next;
    }
}

// ------------------------------------------------------------------- perform

static NSMutableURLRequest *aok_curl_build_request(const struct aok_curl_handle *h,
                                                   CURLcode *err) {
    NSString *urlText = [NSString stringWithUTF8String:h->url];
    NSURL *url = urlText ? [NSURL URLWithString:urlText] : nil;
    if (!url || !url.host) {
        *err = CURLE_URL_MALFORMAT;
        return nil;
    }
    if (!aok_scheme_allowed(url)) {
        *err = CURLE_UNSUPPORTED_PROTOCOL;
        return nil;
    }

    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
    size_t bodylen = h->postfieldsize;
    if (bodylen > h->postfields_cap)
        bodylen = h->postfields_cap;
    if (h->postfields && bodylen > 0) {
        req.HTTPBody = [NSData dataWithBytes:h->postfields length:bodylen];
        req.HTTPMethod = @"POST";
    }
    if (h->method) {
        NSString *m = [NSString stringWithUTF8String:h->method];
        if (m)
            req.HTTPMethod = m;
    }
    if (h->useragent) {
        NSString *ua = [NSString stringWithUTF8String:h->useragent];
        if (ua)
            [req setValue:ua forHTTPHeaderField:@"User-Agent"];
    }
    if (h->accept_encoding && h->accept_encoding[0] != '\0') {
        // An empty string means "whatever you support", which is already what
        // NSURLSession advertises and decodes; a specific one is a real header.
        NSString *enc = [NSString stringWithUTF8String:h->accept_encoding];
        if (enc)
            [req setValue:enc forHTTPHeaderField:@"Accept-Encoding"];
    }
    if (h->userpwd) {
        // Sent up front rather than waiting for a 401. CURLOPT_HTTPAUTH is
        // Basic or ANY here, and ANY with a credential in hand is Basic in
        // every case core.c can produce.
        NSData *raw = [[NSString stringWithUTF8String:h->userpwd]
                          dataUsingEncoding:NSUTF8StringEncoding];
        if (raw) {
            NSString *value = [@"Basic " stringByAppendingString:
                                  [raw base64EncodedStringWithOptions:0]];
            [req setValue:value forHTTPHeaderField:@"Authorization"];
        }
    }
    for (struct curl_slist *n = h->headers; n; n = n->next) {
        // curl's header list is "Name: value", and "Name:" with nothing after
        // it means "remove the header you would otherwise send".
        const char *colon = strchr(n->data, ':');
        if (!colon)
            continue;
        NSString *name = [[NSString alloc] initWithBytes:n->data
                                                  length:(NSUInteger) (colon - n->data)
                                                encoding:NSUTF8StringEncoding];
        const char *v = colon + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        NSString *value = [NSString stringWithUTF8String:v];
        if (name.length == 0)
            continue;
        [req setValue:(value.length ? value : nil) forHTTPHeaderField:name];
    }
    return req;
}

CURLcode curl_easy_perform(CURL *handle) {
    struct aok_curl_handle *h = handle;
    if (!h)
        return CURLE_FAILED_INIT;
    if (!h->url || !h->url[0])
        return CURLE_URL_MALFORMAT;

    @autoreleasepool {
        CURLcode err = CURLE_OK;
        NSMutableURLRequest *req = aok_curl_build_request(h, &err);
        if (!req)
            return err;

        struct aok_curl_xfer *x = calloc(1, sizeof(*x));
        if (!x)
            return CURLE_OUT_OF_MEMORY;
        pthread_mutex_init(&x->mu, NULL);
        pthread_cond_init(&x->cv, NULL);
        x->followlocation = h->followlocation;
        x->maxredirs = h->maxredirs;
        x->ssl_verifypeer = h->ssl_verifypeer;
        x->failonerror = h->failonerror;
        if (h->nobody)
            req.HTTPMethod = @"HEAD";
        h->info_code = 0;
        h->info_redirects = 0;
        h->info_size = 0;
        h->info_http_version = 0;
        free(h->info_url);
        h->info_url = NULL;
        free(h->info_content_type);
        h->info_content_type = NULL;
        struct timespec started;
        clock_gettime(CLOCK_MONOTONIC, &started);
        int headers_sent = 0;

        NSURLSessionConfiguration *config =
            [NSURLSessionConfiguration ephemeralSessionConfiguration];
        // Inactivity, not total time. CURLOPT_LOW_SPEED_TIME is the nearest
        // thing curl has to it -- "give up after this long making no progress"
        // -- so it wins over CONNECTTIMEOUT when it is the tighter of the two.
        NSTimeInterval idle = h->connecttimeout > 0 ? (NSTimeInterval) h->connecttimeout : 60;
        if (h->low_speed_time > 0 && (NSTimeInterval) h->low_speed_time < idle)
            idle = (NSTimeInterval) h->low_speed_time;
        config.timeoutIntervalForRequest = idle;
        config.timeoutIntervalForResource =
            h->timeout > 0 ? (NSTimeInterval) h->timeout : 604800;
        config.HTTPShouldSetCookies = NO;
        config.URLCache = nil;
        config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;

        AOKCurlDelegate *delegate = [AOKCurlDelegate new];
        delegate.xfer = x;
        NSURLSession *session = [NSURLSession sessionWithConfiguration:config
                                                             delegate:delegate
                                                        delegateQueue:nil];
        NSURLSessionDataTask *task = [session dataTaskWithRequest:req];
        [task resume];

        aok_curl_last_detail[0] = '\0';
        CURLcode result = CURLE_OK;
        for (;;) {
            unsigned char *chunk = NULL;
            size_t chunklen = 0;
            int finished;

            pthread_mutex_lock(&x->mu);
            // Headers going out (-i, -D) wait for the protocol, which only
            // the task metrics name and which arrive at the end: the body is
            // held back until then -- or until the buffer is full, when the
            // version is guessed rather than the transfer stalled.
            int hold = (h->headerfn || h->header) && !headers_sent &&
                       !x->protocol[0] && !x->done && !x->suspended;
            if ((x->len == 0 || hold) && !x->done) {
                struct timespec deadline;
                clock_gettime(CLOCK_REALTIME, &deadline);
                deadline.tv_nsec += AOK_CURL_WAIT_SLICE_NS;
                if (deadline.tv_nsec >= 1000000000L) {
                    deadline.tv_sec += 1;
                    deadline.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&x->cv, &x->mu, &deadline);
            }
            hold = (h->headerfn || h->header) && !headers_sent &&
                   !x->protocol[0] && !x->done && !x->suspended;
            if (x->len > 0 && !hold) {
                chunk = x->buf;
                chunklen = x->len;
                x->buf = NULL;
                x->len = 0;
                x->cap = 0;
                if (x->suspended) {
                    x->suspended = 0;
                    [task resume];
                }
            }
            finished = x->done;
            // The header block goes out before the first byte of body, or at
            // the end when there is none (HEAD, -f refusing an error status).
            char *header_block = NULL;
            if (!headers_sent && x->have_response && (chunk || finished)) {
                headers_sent = 1;
                header_block = aok_curl_header_block(x);
            }
            pthread_mutex_unlock(&x->mu);

            if (header_block) {
                size_t hlen = strlen(header_block);
                if (h->headerfn)
                    h->headerfn(header_block, 1, hlen, h->headerdata);
                if (h->header && h->writefn)
                    h->writefn(header_block, 1, hlen, h->writedata);
                free(header_block);
            }
            if (chunk) {
                h->info_size += (long long) chunklen;
                size_t wrote = chunklen;
                if (h->writefn)
                    wrote = h->writefn((char *) chunk, 1, chunklen, h->writedata);
                free(chunk);
                if (wrote != chunklen) {
                    pthread_mutex_lock(&x->mu);
                    if (x->result == CURLE_OK)
                        x->result = CURLE_WRITE_ERROR;
                    pthread_mutex_unlock(&x->mu);
                    [task cancel];
                    result = CURLE_WRITE_ERROR;
                    break;
                }
                continue;   // drain before deciding we are done
            }
            if (finished) {
                pthread_mutex_lock(&x->mu);
                result = x->result;
                pthread_mutex_unlock(&x->mu);
                break;
            }

            // The only place this loop yields to a signal. It may not return:
            // a fatal one tears the task down from here, and the session is
            // left to the invalidate below that then never runs -- the same
            // trade every native program makes at a checkpoint.
            native_checkpoint();
        }

        pthread_mutex_lock(&x->mu);
        if (result != CURLE_OK)
            snprintf(aok_curl_last_detail, sizeof(aok_curl_last_detail), "%s", x->detail);
        h->info_code = x->code;
        h->info_redirects = x->redirects;
        h->info_http_version = x->have_response ? aok_curl_http_version(x->protocol) : 0;
        h->info_url = x->url ? strdup(x->url) : NULL;
        h->info_content_type = x->content_type ? strdup(x->content_type) : NULL;
        pthread_mutex_unlock(&x->mu);
        struct timespec ended;
        clock_gettime(CLOCK_MONOTONIC, &ended);
        h->info_total_time = (double) (ended.tv_sec - started.tv_sec) +
                             (double) (ended.tv_nsec - started.tv_nsec) / 1e9;
        // No frees here: the delegate owns `x` now, and releasing the last
        // reference to it -- ours when this scope ends, or the session's when
        // invalidation completes, whichever is later -- is what frees it.
        [session invalidateAndCancel];
        return result;
    }
}
