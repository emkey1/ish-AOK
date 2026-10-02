#ifndef INCLUDE_features_h__
#define INCLUDE_features_h__

/* Debugging options */

/* #undef GIT_DEBUG_POOL */
/* #undef GIT_DEBUG_STRICT_ALLOC */
/* #undef GIT_DEBUG_STRICT_OPEN */
/* #undef GIT_DEBUG_LEAKCHECK_WIN32 */

/* Feature enablement and provider / backend selection */

#define GIT_THREADS 1
#define GIT_THREADS_PTHREADS 1
/* #undef GIT_THREADS_WIN32 */

#define GIT_SHA1_BUILTIN 1
/* #undef GIT_SHA1_OPENSSL */
/* #undef GIT_SHA1_OPENSSL_FIPS */
/* #undef GIT_SHA1_OPENSSL_DYNAMIC */
/* #undef GIT_SHA1_MBEDTLS */
/* #undef GIT_SHA1_COMMON_CRYPTO */
/* #undef GIT_SHA1_WIN32 */

/* #undef GIT_SHA256_BUILTIN */
/* #undef GIT_SHA256_WIN32 */
#define GIT_SHA256_COMMON_CRYPTO 1
/* #undef GIT_SHA256_OPENSSL */
/* #undef GIT_SHA256_OPENSSL_FIPS */
/* #undef GIT_SHA256_OPENSSL_DYNAMIC */
/* #undef GIT_SHA256_MBEDTLS */

/* #undef GIT_COMPRESSION_BUILTIN */
#define GIT_COMPRESSION_ZLIB 1

#define GIT_NSEC 1
/* #undef GIT_NSEC_MTIM */
#define GIT_NSEC_MTIMESPEC 1
/* #undef GIT_NSEC_MTIME_NSEC */
/* #undef GIT_NSEC_WIN32 */

/* #undef GIT_I18N */
/* #undef GIT_I18N_ICONV */

/* #undef GIT_REGEX_REGCOMP_L */
#define GIT_REGEX_REGCOMP 1
/* #undef GIT_REGEX_PCRE */
/* #undef GIT_REGEX_PCRE2 */
/* #undef GIT_REGEX_BUILTIN */

/* #undef GIT_SSH */
/* #undef GIT_SSH_EXEC */
/* #undef GIT_SSH_LIBSSH2 */
/* #undef GIT_SSH_LIBSSH2_MEMORY_CREDENTIALS */

#define GIT_HTTPS 1
/* #undef GIT_HTTPS_OPENSSL */
/* #undef GIT_HTTPS_OPENSSL_DYNAMIC */
#define GIT_HTTPS_SECURETRANSPORT 1
/* #undef GIT_HTTPS_MBEDTLS */
/* #undef GIT_HTTPS_SCHANNEL */
/* #undef GIT_HTTPS_WINHTTP */

/* #undef GIT_HTTPPARSER_HTTPPARSER */
/* #undef GIT_HTTPPARSER_LLHTTP */
#define GIT_HTTPPARSER_BUILTIN 1

/* #undef GIT_AUTH_NTLM */
/* #undef GIT_AUTH_NTLM_BUILTIN */
/* #undef GIT_AUTH_NTLM_SSPI */

/* #undef GIT_AUTH_NEGOTIATE */
/* #undef GIT_AUTH_NEGOTIATE_GSSFRAMEWORK */
/* #undef GIT_AUTH_NEGOTIATE_GSSAPI */
/* #undef GIT_AUTH_NEGOTIATE_SSPI */

/* Platform details */

#define GIT_ARCH_64 1
/* #undef GIT_ARCH_32 */

#define GIT_QSORT_BSD 1
/* #undef GIT_QSORT_GNU */
/* #undef GIT_QSORT_C11 */
/* #undef GIT_QSORT_MSC */

#define GIT_FUTIMENS 1

/* #undef GIT_RAND_GETENTROPY */
#define GIT_RAND_GETLOADAVG 1

#define GIT_IO_POLL 1
/* #undef GIT_IO_WSAPOLL */
#define GIT_IO_SELECT 1

/* Compile-time information */

#define GIT_BUILD_CPU "arm64"
/* #undef GIT_BUILD_COMMIT */

#endif
