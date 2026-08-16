/* Force-included AFTER kernel/native_libc.h, for the one OpenSSH source that
 * uses a libc name for something of its own.
 *
 * smult_curve25519_ref.c -- djb's curve25519 reference implementation, and the
 * only x25519 there is in a --without-openssl build -- has a file-static
 * helper literally called select(). Two things then go wrong: native_libc.h
 * #defines select onto nlibc_select, turning the definition into a second and
 * wrongly-typed declaration of it, and native_libc.h's own <sys/select.h> has
 * already declared the real select() besides. Undefining is not enough; the
 * local name has to move out of the way.
 *
 * A plain -Dselect=... cannot do this: the -D is on the command line and
 * native_libc.h's #define comes afterwards, so the header simply wins. Being
 * force-included second is the only order that works.
 *
 * Safe only because that file includes NO headers at all -- it is pure
 * arithmetic -- so nothing later in the translation unit can be renamed by
 * accident. Only ever rename a name the file DEFINES rather than calls: a call
 * left unrouted would reach the host, though not silently for long, since
 * tools/check-native-libc.py fails the build on an unrouted external.
 */
#ifndef AOK_OPENSSH_LOCAL_NAMES_H
#define AOK_OPENSSH_LOCAL_NAMES_H

#undef select
#define select aok_curve25519_ref_select

#endif
