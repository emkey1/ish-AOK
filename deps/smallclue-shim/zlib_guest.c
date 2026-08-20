// Implementation of deps/smallclue-shim/zlib.h -- read that first for why the
// gz* layer is reimplemented rather than linked.
//
// Everything here is deliberately compiled INSIDE libsmallclue, with
// kernel/native_libc.h force-included, so that the open/read/write/close below
// are the redirected ones and the descriptors are the guest's. Moving this
// file to a target without that force-include would compile cleanly and be
// silently wrong, which is the failure mode the header exists to prevent.

#define AOK_ZLIB_SHIM_NO_REDIRECT
#include <zlib.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AOK_GZ_BUF 32768

struct aok_gz_stream {
    int fd;
    int writing;
    int eof;          // no more bytes from the descriptor
    int done;         // read: the stream ended and we stopped
    int saw_member;   // read: at least one complete gzip member decoded
    int err;          // Z_* code, or 0
    const char *msg;
    z_stream zs;
    unsigned char buf[AOK_GZ_BUF];  // raw input when reading, staged output when writing
};

static void gzSetError(struct aok_gz_stream *gz, int err, const char *msg) {
    gz->err = err;
    gz->msg = msg;
}

// write(2) is free to write less than asked. Every byte here is compressed
// output, so a short write that is not retried is silent corruption of the
// archive rather than a visible error.
static int gzWriteFully(struct aok_gz_stream *gz, const unsigned char *p, size_t len) {
    while (len > 0) {
        ssize_t n = write(gz->fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            gzSetError(gz, Z_ERRNO, strerror(errno));
            return -1;
        }
        if (n == 0) {
            gzSetError(gz, Z_ERRNO, "short write");
            return -1;
        }
        p += (size_t) n;
        len -= (size_t) n;
    }
    return 0;
}

// Hand deflate an empty input and drain whatever it produces. flush is
// Z_NO_FLUSH while writing and Z_FINISH from gzclose.
static int gzDrainDeflate(struct aok_gz_stream *gz, int flush) {
    for (;;) {
        gz->zs.next_out = gz->buf;
        gz->zs.avail_out = sizeof(gz->buf);
        int ret = deflate(&gz->zs, flush);
        if (ret == Z_STREAM_ERROR) {
            gzSetError(gz, ret, "deflate failed");
            return -1;
        }
        size_t have = sizeof(gz->buf) - gz->zs.avail_out;
        if (have > 0 && gzWriteFully(gz, gz->buf, have) < 0)
            return -1;
        if (flush == Z_FINISH) {
            if (ret == Z_STREAM_END)
                return 0;
            // Z_OK with a full output buffer means more to come; Z_BUF_ERROR
            // with nothing produced means it is wedged.
            if (have == 0 && ret != Z_OK) {
                gzSetError(gz, ret, "deflate did not finish");
                return -1;
            }
            continue;
        }
        if (gz->zs.avail_in == 0 && gz->zs.avail_out != 0)
            return 0;
    }
}

// mode is "r"/"w" with an optional 'b' and, for writing, a level digit.
// Returns -1 if it names neither direction.
static int gzParseMode(const char *mode, int *level) {
    int writing = -1;
    *level = Z_DEFAULT_COMPRESSION;
    for (const char *p = mode; p && *p; p++) {
        if (*p == 'r')
            writing = 0;
        else if (*p == 'w' || *p == 'a')
            writing = 1;
        else if (*p >= '0' && *p <= '9')
            *level = *p - '0';
        // 'b' and anything else: the guest has no text mode and no
        // transparent-write mode, so there is nothing to honour.
    }
    return writing;
}

static struct aok_gz_stream *gzNew(int fd, const char *mode) {
    int level;
    int writing = gzParseMode(mode, &level);
    if (writing < 0) {
        errno = EINVAL;
        return NULL;
    }

    struct aok_gz_stream *gz = calloc(1, sizeof(*gz));
    if (!gz) {
        errno = ENOMEM;
        return NULL;
    }
    gz->fd = fd;
    gz->writing = writing;

    int ret;
    if (writing) {
        // windowBits 15 + 16 asks deflate for a gzip wrapper rather than a
        // zlib one, which is what makes the output a .gz file.
        ret = deflateInit2(&gz->zs, level, Z_DEFLATED, 15 + 16, 8,
                           Z_DEFAULT_STRATEGY);
    } else {
        // 15 + 32 auto-detects gzip or zlib framing.
        ret = inflateInit2(&gz->zs, 15 + 32);
    }
    if (ret != Z_OK) {
        free(gz);
        errno = (ret == Z_MEM_ERROR) ? ENOMEM : EINVAL;
        return NULL;
    }
    return gz;
}

aok_gzFile aok_gzopen(const char *path, const char *mode) {
    int level;
    int writing = gzParseMode(mode, &level);
    if (writing < 0) {
        errno = EINVAL;
        return NULL;
    }
    int flags = writing ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
    int fd = open(path, flags, 0666);
    if (fd < 0)
        return NULL;  // errno is the guest's, which is what the caller prints
    struct aok_gz_stream *gz = gzNew(fd, mode);
    if (!gz) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }
    return gz;
}

aok_gzFile aok_gzdopen(int fd, const char *mode) {
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }
    return gzNew(fd, mode);
}

int aok_gzread(aok_gzFile file, void *buf, unsigned len) {
    struct aok_gz_stream *gz = file;
    if (!gz || gz->writing) {
        errno = EBADF;
        return -1;
    }
    if (len == 0)
        return 0;
    if (gz->err != 0 && gz->err != Z_ERRNO)
        return -1;

    gz->zs.next_out = buf;
    gz->zs.avail_out = len;

    // Fill the caller's buffer completely unless the stream ends first: tar
    // reads fixed 512-byte blocks and treats a short read as a truncated
    // archive.
    while (gz->zs.avail_out > 0 && !gz->done) {
        if (gz->zs.avail_in == 0 && !gz->eof) {
            ssize_t n = read(gz->fd, gz->buf, sizeof(gz->buf));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                gzSetError(gz, Z_ERRNO, strerror(errno));
                break;
            }
            if (n == 0)
                gz->eof = 1;
            gz->zs.next_in = gz->buf;
            gz->zs.avail_in = (unsigned) n;
        }

        int ret = inflate(&gz->zs, Z_NO_FLUSH);

        if (ret == Z_STREAM_END) {
            // A .gz file may hold several members back to back -- `cat a.gz
            // b.gz` is a valid archive and gzip reads it whole -- so a
            // finished member is not necessarily a finished file.
            gz->saw_member = 1;
            if ((gz->zs.avail_in > 0 || !gz->eof) &&
                inflateReset(&gz->zs) == Z_OK)
                continue;
            gz->done = 1;
            break;
        }
        if (ret == Z_OK)
            continue;
        if (ret == Z_BUF_ERROR) {
            // No progress possible. With input still to come that just means
            // "refill me"; at end of input it means the stream was truncated.
            if (!gz->eof || gz->zs.avail_in > 0)
                continue;
            gz->done = 1;
            if (!gz->saw_member)
                gzSetError(gz, Z_DATA_ERROR, "unexpected end of file");
            break;
        }
        // Z_DATA_ERROR, Z_MEM_ERROR, Z_NEED_DICT. Bytes trailing a complete
        // member are padding, which gzip tolerates; bytes instead of one are
        // not a gzip file at all.
        gz->done = 1;
        if (!gz->saw_member)
            gzSetError(gz, ret, gz->zs.msg ? gz->zs.msg : "not in gzip format");
        break;
    }

    // zlib hands back the bytes it did decode and reports the error on the
    // NEXT call, which is what lets a caller keep the good prefix of a
    // truncated archive. gz->err survives to make that happen.
    unsigned got = len - gz->zs.avail_out;
    if (got > 0)
        return (int) got;
    return gz->err != 0 ? -1 : 0;
}

int aok_gzwrite(aok_gzFile file, const void *buf, unsigned len) {
    struct aok_gz_stream *gz = file;
    if (!gz || !gz->writing) {
        errno = EBADF;
        return 0;
    }
    if (len == 0)
        return 0;
    gz->zs.next_in = (Bytef *) (uintptr_t) buf;
    gz->zs.avail_in = len;
    if (gzDrainDeflate(gz, Z_NO_FLUSH) < 0)
        return 0;
    return (int) len;
}

int aok_gzclose(aok_gzFile file) {
    struct aok_gz_stream *gz = file;
    if (!gz)
        return Z_STREAM_ERROR;

    int ret = Z_OK;
    if (gz->writing) {
        gz->zs.next_in = NULL;
        gz->zs.avail_in = 0;
        if (gzDrainDeflate(gz, Z_FINISH) < 0)
            ret = gz->err ? gz->err : Z_ERRNO;
        deflateEnd(&gz->zs);
    } else {
        inflateEnd(&gz->zs);
    }
    if (close(gz->fd) < 0 && ret == Z_OK)
        ret = Z_ERRNO;
    free(gz);
    return ret;
}

const char *aok_gzerror(aok_gzFile file, int *errnum) {
    struct aok_gz_stream *gz = file;
    if (!gz) {
        if (errnum)
            *errnum = Z_STREAM_ERROR;
        return "invalid stream";
    }
    if (errnum)
        *errnum = gz->err;
    return gz->msg ? gz->msg : "";
}
