/* Hand-written config.h for liblzma 5.8.3 vendored into AOK-Workstation.
 * Target: Apple platforms (iOS/macOS), arm64 + x86_64, decoders + encoders,
 * single-threaded, size-optimized (runtime tables, no generated headers).
 * Encoders are built because libarchive's xz/7zip/xar *writer* object files
 * reference the lzma_*_encoder entry points at link time (they compile under
 * HAVE_LZMA_H, which the xz reader requires) even though the app never encodes
 * at runtime — the symbols must be DEFINED in our own binary.
 * This replaces the autotools-generated config.h. */
#ifndef LZMA_VENDORED_CONFIG_H
#define LZMA_VENDORED_CONFIG_H

/* --- standard / platform headers (present on Darwin) --- */
#define STDC_HEADERS 1
#define HAVE_STDBOOL_H 1
#define HAVE__BOOL 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_LIMITS_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_SYSCTL_H 1

/* --- sizes / endianness (arm64 & x86_64 are little-endian) --- */
#define SIZEOF_SIZE_T 8
/* WORDS_BIGENDIAN intentionally undefined */

/* --- time --- */
#define HAVE_CLOCK_GETTIME 1
#define HAVE_CLOCK_MONOTONIC 1
#define HAVE_DECL_CLOCK_MONOTONIC 1
#define HAVE_NANOSLEEP 1

/* --- compiler features (clang) --- */
#define HAVE_FUNC_ATTRIBUTE_CONSTRUCTOR 1
#define HAVE_VISIBILITY 1

/* --- size-optimized: runtime CRC/probability tables (no *_table.c generated headers) --- */
#define HAVE_SMALL 1

/* --- integrity checks --- */
#define HAVE_CHECK_CRC32 1
#define HAVE_CHECK_CRC64 1
#define HAVE_CHECK_SHA256 1

/* --- decoders (used at runtime to import .tar.xz roots) --- */
#define HAVE_DECODERS 1
#define HAVE_DECODER_LZMA1 1
#define HAVE_DECODER_LZMA2 1
#define HAVE_DECODER_DELTA 1
#define HAVE_DECODER_X86 1
#define HAVE_DECODER_ARM 1
#define HAVE_DECODER_ARMTHUMB 1
#define HAVE_DECODER_ARM64 1
#define HAVE_DECODER_POWERPC 1
#define HAVE_DECODER_IA64 1
#define HAVE_DECODER_SPARC 1
#define HAVE_DECODER_RISCV 1

/* --- encoders (not used at runtime, but libarchive's xz/7zip/xar writers
 *     reference the encoder entry points at link time; see file header) --- */
#define HAVE_ENCODERS 1
#define HAVE_ENCODER_LZMA1 1
#define HAVE_ENCODER_LZMA2 1
#define HAVE_ENCODER_DELTA 1
#define HAVE_ENCODER_X86 1
#define HAVE_ENCODER_ARM 1
#define HAVE_ENCODER_ARMTHUMB 1
#define HAVE_ENCODER_ARM64 1
#define HAVE_ENCODER_POWERPC 1
#define HAVE_ENCODER_IA64 1
#define HAVE_ENCODER_SPARC 1
#define HAVE_ENCODER_RISCV 1

/* --- match finders (required by the LZMA encoder) --- */
#define HAVE_MF_HC3 1
#define HAVE_MF_HC4 1
#define HAVE_MF_BT2 1
#define HAVE_MF_BT3 1
#define HAVE_MF_BT4 1

/* --- single-threaded: no MYTHREAD_* backend defined --- */

/* --- physmem probe fallback (MiB) --- */
#define ASSUME_RAM 128

/* --- namespace tuklib internal symbols under the liblzma prefix --- */
#define TUKLIB_SYMBOL_PREFIX lzma_

/* --- package identity --- */
#define PACKAGE_NAME "xz"
#define PACKAGE_VERSION "5.8.3"
#define PACKAGE_BUGREPORT "xz@tukaani.org"
#define PACKAGE_URL "https://tukaani.org/xz/"

#endif /* LZMA_VENDORED_CONFIG_H */
