// sshd for native mode.
//
// ssh, scp, sftp and ssh-keygen are NATIVE programs: host code compiled into
// the app and dispatched from the guest's execve. sshd cannot be one, and the
// reason is a single line in sshd-session.c:
//
//     pmonitor->m_pid = fork();
//
// a fork-and-continue, with both sides carrying on in sshd's own C code. A
// native program is a function call on a guest task's thread inside the app's
// ONE address space, so it cannot have two sides see different memory at the
// same addresses -- structurally the same wall the bash port hit. An ordinary
// emulated guest ELF forks normally, so the problem does not arise. The cost
// is connection setup and per-packet crypto speed, which is real for scp
// throughput and marginal for typing at a prompt, because the shell on the far
// end is native zsh either way.
//
// Native mode has no distribution in it: /lib is empty and there is no libc to
// link against, so the binaries are static. They are built from the SAME
// vendored OpenSSH tree the native client comes from, by
// tools/build-guest-sshd.sh, which also explains why they are three binaries
// and not one.
//
// 21 MB of static aarch64 ELF compresses to 2.5 MB as one LZMA tar, so that is
// what ships: fs/aok.c serves the archive from the app bundle at
// /AOK/tools/guest-sshd-aarch64.tar.lzma, and this unpacks it into the root the
// first time a build runs. Unpacking into the root rather than running from
// /AOK is deliberate -- sshd checks the ownership and mode of what it execs,
// and the root is where a user can replace it with their own build.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/guest_sshd.h"
#include "kernel/hostinfo.h"
#include "platform/platform.h"
#include "debug.h"

// Where the three roles go. sshd execs the other two by the paths compiled
// into it, so this is not a free choice: see tools/build-guest-sshd.sh.
static const struct {
    const char *member;
    const char *dest;
} guest_sshd_files[] = {
    {"sshd",         "/usr/sbin/sshd"},
    {"sshd-session", "/usr/libexec/sshd-session"},
    {"sshd-auth",    "/usr/libexec/sshd-auth"},
};
#define GUEST_SSHD_FILE_COUNT \
    (sizeof(guest_sshd_files) / sizeof(guest_sshd_files[0]))

// The ustar header, which is all of tar this needs.
// tools/build-guest-sshd.sh writes --format=ustar deliberately: bsdtar's
// default pax format interleaves 'x' extended headers, and while the loop
// below skips those, plain ustar is one less thing to be surprised by.
struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};
_Static_assert(sizeof(struct ustar_header) == 512, "ustar header is one block");

#define USTAR_BLOCK 512

// A tar numeric field: octal digits, then a NUL or a space, and some writers
// pad with leading spaces. Returns false on a field that is not that, so a
// corrupt archive stops the walk instead of seeking to a wild offset.
static bool parse_octal(const char *field, size_t size, uint64_t *out) {
    uint64_t value = 0;
    size_t i = 0;
    while (i < size && field[i] == ' ')
        i++;
    if (i == size || field[i] < '0' || field[i] > '7')
        return false;
    for (; i < size; i++) {
        char c = field[i];
        if (c == '\0' || c == ' ')
            break;
        if (c < '0' || c > '7')
            return false;
        if (value > (UINT64_MAX - (uint64_t) (c - '0')) / 8)
            return false;
        value = value * 8 + (uint64_t) (c - '0');
    }
    *out = value;
    return true;
}

static bool path_exists(const char *path) {
    struct statbuf stat;
    return generic_statat(AT_PWD, path, &stat, AT_SYMLINK_NOFOLLOW_) == 0;
}

// The archive, through the guest VFS, as the running task -- so the CLI and
// the app read it identically. Deliberately not native_root.c's read_file:
// that one caps at 1 MB, and this is 2.5.
#define ARCHIVE_MAX (32u * 1024 * 1024)

static char *read_archive(const char *path, size_t *len_out) {
    *len_out = 0;
    struct fd *fd = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(fd))
        return NULL;
    size_t cap = 1 << 20, len = 0;
    char *buf = malloc(cap);
    while (buf != NULL) {
        if (len == cap) {
            if (cap >= ARCHIVE_MAX) {
                free(buf);
                buf = NULL;
                break;
            }
            char *bigger = realloc(buf, cap * 2);
            if (bigger == NULL) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = bigger;
            cap *= 2;
        }
        ssize_t n = generic_read(fd, buf + len, cap - len);
        if (n < 0) {
            free(buf);
            buf = NULL;
            break;
        }
        if (n == 0)
            break;
        len += (size_t) n;
    }
    fd_close(fd);
    if (buf == NULL || len == 0) {
        free(buf);
        return NULL;
    }
    *len_out = len;
    return buf;
}

// Write LEN bytes to PATH with MODE, through a temporary and a rename, so a
// reader -- or an sshd already running from the old copy -- never sees half a
// binary. The directory is made if it is missing: /usr/libexec is not part of
// a root's skeleton on a root provisioned by an older build.
static int write_binary(const char *path, const char *data, size_t len,
                        mode_t_ mode) {
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash != NULL && slash != dir) {
        *slash = '\0';
        if (!path_exists(dir))
            generic_mkdirat(AT_PWD, dir, 0755);
    }

    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.aok-new", path);
    generic_unlinkat(AT_PWD, tmp);
    struct fd *fd = generic_open(tmp, O_WRONLY_ | O_CREAT_ | O_EXCL_ | O_NOFOLLOW_, mode);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);

    size_t done = 0;
    int err = 0;
    while (done < len) {
        ssize_t n = generic_write(fd, data + done, len - done);
        if (n <= 0) {
            err = n < 0 ? (int) n : _EIO;
            break;
        }
        done += (size_t) n;
    }
    fd_close(fd);

    if (err == 0)
        err = generic_setattrat(AT_PWD, tmp, make_attr(mode, mode), false);
    if (err == 0)
        err = generic_renameat(AT_PWD, tmp, AT_PWD, path, 0);
    if (err < 0)
        generic_unlinkat(AT_PWD, tmp);
    return err;
}

// The destination for a member name, or NULL for one this does not want. Only
// the basename is compared, so a tar made with a leading ./ or a directory
// prefix still lands correctly.
static const char *dest_for_member(const char *name) {
    const char *slash = strrchr(name, '/');
    const char *base = slash != NULL ? slash + 1 : name;
    for (size_t i = 0; i < GUEST_SSHD_FILE_COUNT; i++) {
        if (strcmp(base, guest_sshd_files[i].member) == 0)
            return guest_sshd_files[i].dest;
    }
    return NULL;
}

static long installed_build(void) {
    struct fd *fd = generic_open(GUEST_SSHD_STAMP, O_RDONLY_, 0);
    if (IS_ERR(fd))
        return -1;
    char buf[64] = {};
    ssize_t n = generic_read(fd, buf, sizeof(buf) - 1);
    fd_close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return strtol(buf, NULL, 10);
}

bool guest_sshd_installed(void) {
    for (size_t i = 0; i < GUEST_SSHD_FILE_COUNT; i++) {
        if (!path_exists(guest_sshd_files[i].dest))
            return false;
    }
    return installed_build() >= (long) buildTimestamp();
}

int guest_sshd_install(bool force) {
    if (!force && guest_sshd_installed())
        return 0;
    if (!path_exists(GUEST_SSHD_ARCHIVE_PATH))
        return _ENOENT;

    size_t packed_len = 0;
    char *packed = read_archive(GUEST_SSHD_ARCHIVE_PATH, &packed_len);
    if (packed == NULL)
        return _EIO;

    size_t tar_len = 0;
    char *tar = host_lzma_decompress(packed, packed_len, &tar_len);
    free(packed);
    if (tar == NULL)
        return _ENOSYS;

    // Walk the blocks. Every member this wants must be found, or the install
    // is reported as a failure rather than leaving two of three binaries in
    // place for sshd to fail on at the first connection.
    unsigned found = 0;
    int err = 0;
    size_t off = 0;
    while (off + USTAR_BLOCK <= tar_len) {
        const struct ustar_header *header =
            (const struct ustar_header *) (tar + off);
        if (header->name[0] == '\0')
            break; // the end-of-archive block

        uint64_t size = 0;
        if (!parse_octal(header->size, sizeof(header->size), &size)) {
            err = _EINVAL;
            break;
        }
        size_t data_off = off + USTAR_BLOCK;
        if (size > tar_len - data_off) {
            err = _EINVAL;
            break;
        }

        if (header->typeflag == '0' || header->typeflag == '\0') {
            char name[sizeof(header->name) + 1];
            memcpy(name, header->name, sizeof(header->name));
            name[sizeof(header->name)] = '\0';
            const char *dest = dest_for_member(name);
            if (dest != NULL) {
                err = write_binary(dest, tar + data_off, (size_t) size, 0755);
                if (err < 0)
                    break;
                found++;
            }
        }

        // Member data is padded out to a whole block.
        off = data_off + (((size_t) size + USTAR_BLOCK - 1) & ~(size_t) (USTAR_BLOCK - 1));
    }
    free(tar);

    if (err == 0 && found != GUEST_SSHD_FILE_COUNT)
        err = _EINVAL;
    if (err < 0) {
        printk("guest sshd: install failed (%d, %u of %zu members)\n",
               err, found, GUEST_SSHD_FILE_COUNT);
        return err;
    }

    char stamp[64];
    int len = snprintf(stamp, sizeof(stamp), "%ld\n", (long) buildTimestamp());
    err = write_binary(GUEST_SSHD_STAMP, stamp, (size_t) len, 0644);
    if (err < 0)
        return err;
    printk("guest sshd: installed %zu binaries\n", GUEST_SSHD_FILE_COUNT);
    return 0;
}
