/* <rpc/types.h> for platforms that do not have one.
 *
 * OpenSSH's includes.h includes this header behind HAVE_RPC_TYPES_H, and says
 * why: "For INADDR_LOOPBACK". Its config.h was generated on Darwin, which has
 * the header; Debian and friends put it in libtirpc, under /usr/include/tirpc,
 * where an unqualified <rpc/types.h> does not find it even when installed. So
 * 181 of the 186 Linux build failures were this one line.
 *
 * INADDR_LOOPBACK comes from <netinet/in.h> on Linux -- verified, not assumed:
 *
 *     #include <netinet/in.h>
 *     printf("%08x", INADDR_LOOPBACK);   ->  7f000001
 *
 * so supplying it from there gives OpenSSH exactly what it asked for. This
 * directory is on the include path only when the host is not Darwin, so the
 * shipping build never sees this file and keeps using the real header.
 *
 * Same arrangement as deps/smallclue-shim/openssl/evp.h, which stands in for a
 * header OpenSSH expects and this build does not otherwise provide.
 */
#ifndef AOK_SHIM_RPC_TYPES_H
#define AOK_SHIM_RPC_TYPES_H
#include <netinet/in.h>
#endif
