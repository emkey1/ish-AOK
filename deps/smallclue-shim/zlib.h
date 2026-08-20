// Guest-backed gzip streams for deps/smallclue/src/{tar,gzip}_app.c.
//
// WHY THIS EXISTS. tar, gzip, gunzip and zcat were stubbed out, and the
// obvious diagnosis -- "AOK never links zlib" -- was wrong twice over. zlib IS
// public on iOS as well as macOS, so the link line was always one `-lz` away;
// and adding it would have shipped a worse bug than the stub.
//
// The reason is that zlib is a system dylib compiled against the HOST libc.
// kernel/native_libc.h redirects a native program's open/read/write onto the
// guest's VFS by rewriting the names AOK's own translation units call, which
// does nothing whatsoever to calls made from inside a prebuilt dylib. So
// gzopen("/etc/x.gz") opens iOS's /etc/x.gz, and gzdopen(fd) hands a guest
// descriptor number to the host's read(2), where it names an unrelated file or
// nothing at all.
//
// That failure is invisible exactly where it is most dangerous. Any path that
// exists on both sides round-trips perfectly -- gzipping something under /tmp
// looks flawless, because zlib quietly did the whole operation on the HOST's
// /tmp and the guest's /tmp is where the caller then looked. A stub that
// refuses honestly is better than that, which is why one stood here for so
// long.
//
// WHAT CHANGES IT. The split in zlib's API is clean and it falls in our
// favour. The gz* family is the only part that does I/O; deflate/inflate are
// pure transforms over caller-owned buffers, no descriptor, no path, no host
// state -- they qualify for tools/check-native-libc.py's allowlist on the same
// terms as memcpy. So the gz* file layer is reimplemented here over the
// redirected open/read/write/close, and the compression itself stays zlib's.
// The bytes are the guest's; the algorithm is the library's.
//
// SCOPE. The six functions and one type those two applets use, and nothing
// else -- not gzprintf, gzgets, gzseek or gztell. A third consumer wanting
// more should extend this deliberately, having re-read the paragraph above;
// anything missing is a compile error naming the symbol, which is the correct
// outcome.

#ifndef AOK_SMALLCLUE_SHIM_ZLIB_H
#define AOK_SMALLCLUE_SHIM_ZLIB_H

// The real header, for deflate/inflate and the Z_* constants. #include_next
// resumes the search after this directory, so it finds the SDK's.
#include_next <zlib.h>

typedef struct aok_gz_stream *aok_gzFile;

// mode is "r" or "w", optionally with 'b' (ignored -- the guest has no text
// mode) and, for writing, a single digit giving the compression level.
// Returns NULL with errno set, which is what gzip_app.c prints.
aok_gzFile aok_gzopen(const char *path, const char *mode);

// Takes ownership of fd: aok_gzclose closes it. fd is a GUEST descriptor,
// which is the whole point -- it arrives from the redirected fileno/dup.
aok_gzFile aok_gzdopen(int fd, const char *mode);

// Returns bytes read, short only at end of stream (tar reads fixed 512-byte
// blocks and would mis-parse a short read), 0 at EOF, -1 on error.
int aok_gzread(aok_gzFile file, void *buf, unsigned len);

// Returns len on success, 0 on error -- as zlib does.
int aok_gzwrite(aok_gzFile file, const void *buf, unsigned len);

// Finishes the stream, closes the descriptor and frees the handle.
int aok_gzclose(aok_gzFile file);

const char *aok_gzerror(aok_gzFile file, int *errnum);

#ifndef AOK_ZLIB_SHIM_NO_REDIRECT
#define gzFile aok_gzFile
#define gzopen aok_gzopen
#define gzdopen aok_gzdopen
#define gzread aok_gzread
#define gzwrite aok_gzwrite
#define gzclose aok_gzclose
#define gzerror aok_gzerror
#endif

#endif
