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
    pthread_cond_destroy(&x->cv);
    pthread_mutex_destroy(&x->mu);
    free(x);
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
    if (error && x->result == CURLE_OK) {
        switch (error.code) {
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
    free(h);
}

const char *curl_easy_strerror(CURLcode code) {
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

        CURLcode result = CURLE_OK;
        for (;;) {
            unsigned char *chunk = NULL;
            size_t chunklen = 0;
            int finished;

            pthread_mutex_lock(&x->mu);
            if (x->len == 0 && !x->done) {
                struct timespec deadline;
                clock_gettime(CLOCK_REALTIME, &deadline);
                deadline.tv_nsec += AOK_CURL_WAIT_SLICE_NS;
                if (deadline.tv_nsec >= 1000000000L) {
                    deadline.tv_sec += 1;
                    deadline.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&x->cv, &x->mu, &deadline);
            }
            if (x->len > 0) {
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
            pthread_mutex_unlock(&x->mu);

            if (chunk) {
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

        // No frees here: the delegate owns `x` now, and releasing the last
        // reference to it -- ours when this scope ends, or the session's when
        // invalidation completes, whichever is later -- is what frees it.
        [session invalidateAndCancel];
        return result;
    }
}
