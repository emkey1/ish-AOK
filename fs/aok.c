#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"

#define AOKFS_MAGIC 0x414f4b31

enum aokfs_node_kind {
    aokfs_root = 1,
    aokfs_readme,
    aokfs_version,
    aokfs_persist_dir,
    aokfs_roots_dir,
    aokfs_fixes_dir,
    aokfs_fixes_devuan_dir,
    aokfs_fixes_devuan_readme,
    aokfs_fixes_devuan_pkcsslotd_init,
    aokfs_fixes_debian_link,
    aokfs_tests_dir,
    // Individual /tests/* files are no longer enumerated here -- they come from
    // the build-time generator (tools/gen-aokfs.py + fs/aok-tests.manifest) and
    // are addressed as generated nodes (see AOKFS_GEN_BASE below).
    aokfs_tools_dir,
    aokfs_tools_ish_benchmark,
    aokfs_tools_setup_ish_benchmark,
    aokfs_tests_audio_dir,
    aokfs_audio_raw,
    aokfs_audio_wav,
    // Arch-specific test subdirectories: their FILES come from the generated
    // table (manifest names like "x86/atomics32.c" become paths under
    // /tests/x86/); only the directory nodes themselves are enumerated here.
    aokfs_tests_x86_dir,
    aokfs_tests_arm64_dir,
    // ktop's source/build-script subdirectory: same pattern as the arch test
    // dirs above, but rooted at /tools/ktop/ against the *tools* generated
    // table (manifest names like "ktop/ktop.c" become paths under /tools/ktop/).
    aokfs_tools_ktop_dir,
};

static enum aokfs_node_kind aokfs_decode_node(void *fs_data) {
    return (enum aokfs_node_kind) (uintptr_t) fs_data;
}

static void *aokfs_encode_node(enum aokfs_node_kind node) {
    return (void *) (uintptr_t) node;
}

// The /tests/* and /tools/* files are generated at build time from
// fs/aok-tests.manifest and fs/aok-tools.manifest by tools/gen-aokfs.py. Each is
// addressed as a "generated node" whose id is a per-table base plus its index in
// the generated table. This keeps the hand-written enum machinery for
// directories, symlinks, and the bundled binary blobs, while the (frequently-
// changing) embedded sources are picked up automatically.
#include "aok_generated_tests.inc"
#include "aok_generated_tools.inc"
#define AOKFS_GEN_BASE 0x10000
#define AOKFS_GEN_TOOLS_BASE 0x20000
static bool aokfs_node_is_gen_tools(enum aokfs_node_kind node) {
    return (unsigned) node >= AOKFS_GEN_TOOLS_BASE &&
        (unsigned) node < AOKFS_GEN_TOOLS_BASE + AOKFS_GEN_FILE_COUNT_tools;
}
static bool aokfs_node_is_gen(enum aokfs_node_kind node) {
    return ((unsigned) node >= AOKFS_GEN_BASE &&
            (unsigned) node < AOKFS_GEN_BASE + AOKFS_GEN_FILE_COUNT) ||
        aokfs_node_is_gen_tools(node);
}
static const struct aokfs_gen_file *aokfs_gen_entry(enum aokfs_node_kind node) {
    if (aokfs_node_is_gen_tools(node))
        return &aokfs_gen_files_tools[(unsigned) node - AOKFS_GEN_TOOLS_BASE];
    return &aokfs_gen_files[(unsigned) node - AOKFS_GEN_BASE];
}

static bool aokfs_node_is_dir(enum aokfs_node_kind node) {
    return node == aokfs_root ||
        node == aokfs_fixes_dir ||
        node == aokfs_persist_dir ||
        node == aokfs_roots_dir ||
        node == aokfs_fixes_devuan_dir ||
        node == aokfs_tools_dir ||
        node == aokfs_tests_dir ||
        node == aokfs_tests_audio_dir ||
        node == aokfs_tests_x86_dir ||
        node == aokfs_tests_arm64_dir ||
        node == aokfs_tools_ktop_dir;
}

static bool aokfs_node_is_symlink(enum aokfs_node_kind node) {
    return node == aokfs_fixes_debian_link;
}

static bool aokfs_node_is_bundled_file(enum aokfs_node_kind node) {
    return node == aokfs_audio_raw ||
        node == aokfs_audio_wav ||
        node == aokfs_tools_ish_benchmark;
}

static mode_t_ aokfs_node_mode(enum aokfs_node_kind node) {
    if (aokfs_node_is_gen(node))
        return S_IFREG | (aokfs_gen_entry(node)->mode & 07777);
    if (node == aokfs_persist_dir || node == aokfs_roots_dir)
        return S_IFDIR | 0777;
    if (aokfs_node_is_dir(node))
        return S_IFDIR | 0555;
    if (aokfs_node_is_symlink(node))
        return S_IFLNK | 0777;
    if (node == aokfs_tools_setup_ish_benchmark)
        return S_IFREG | 0555;
    return S_IFREG | 0444;
}

static qword_t aokfs_node_inode(enum aokfs_node_kind node) {
    return (qword_t) node;
}

static const char *aokfs_node_path(enum aokfs_node_kind node) {
    if (aokfs_node_is_gen(node))
        return aokfs_gen_entry(node)->path;
    switch (node) {
        case aokfs_root:
            return "";
        case aokfs_readme:
            return "/README.txt";
        case aokfs_version:
            return "/VERSION";
        case aokfs_persist_dir:
            return "/persist";
        case aokfs_roots_dir:
            return "/roots";
        case aokfs_fixes_dir:
            return "/fixes";
        case aokfs_fixes_devuan_dir:
            return "/fixes/devuan";
        case aokfs_fixes_devuan_readme:
            return "/fixes/devuan/README.txt";
        case aokfs_fixes_devuan_pkcsslotd_init:
            return "/fixes/devuan/fix-pkcsslotd-init.sh";
        case aokfs_fixes_debian_link:
            return "/fixes/debian";
        case aokfs_tests_dir:
            return "/tests";
        case aokfs_tests_x86_dir:
            return "/tests/x86";
        case aokfs_tests_arm64_dir:
            return "/tests/arm64";
        case aokfs_tools_dir:
            return "/tools";
        case aokfs_tools_ish_benchmark:
            return "/tools/iSH_benchmark.tgz";
        case aokfs_tools_setup_ish_benchmark:
            return "/tools/setup-ish-benchmark.sh";
        case aokfs_tools_ktop_dir:
            return "/tools/ktop";
        case aokfs_tests_audio_dir:
            return "/tests/audio";
        case aokfs_audio_raw:
            return "/tests/audio/test-tone-48k-s16le-stereo.raw";
        case aokfs_audio_wav:
            return "/tests/audio/test-tone-48k-s16le-stereo.wav";
    }
    return "";
}

static const char *aokfs_node_basename(enum aokfs_node_kind node) {
    const char *path = aokfs_node_path(node);
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static bool aokfs_lookup_node(const char *path, enum aokfs_node_kind *node_out) {
    static const enum aokfs_node_kind nodes[] = {
        aokfs_root,
        aokfs_readme,
        aokfs_version,
        aokfs_persist_dir,
        aokfs_roots_dir,
        aokfs_fixes_dir,
        aokfs_fixes_devuan_dir,
        aokfs_fixes_devuan_readme,
        aokfs_fixes_devuan_pkcsslotd_init,
        aokfs_fixes_debian_link,
        aokfs_tests_dir,
        aokfs_tests_x86_dir,
        aokfs_tests_arm64_dir,
        aokfs_tools_dir,
        aokfs_tools_ish_benchmark,
        aokfs_tools_setup_ish_benchmark,
        aokfs_tools_ktop_dir,
        aokfs_tests_audio_dir,
        aokfs_audio_raw,
        aokfs_audio_wav,
    };

    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        enum aokfs_node_kind node = nodes[i];
        if (strcmp(path, aokfs_node_path(node)) == 0) {
            *node_out = node;
            return true;
        }
    }
    // Generated /tests/* files.
    for (size_t i = 0; i < AOKFS_GEN_FILE_COUNT; i++) {
        if (strcmp(path, aokfs_gen_files[i].path) == 0) {
            *node_out = (enum aokfs_node_kind) (AOKFS_GEN_BASE + i);
            return true;
        }
    }
    // Generated /tools/* files.
    for (size_t i = 0; i < AOKFS_GEN_FILE_COUNT_tools; i++) {
        if (strcmp(path, aokfs_gen_files_tools[i].path) == 0) {
            *node_out = (enum aokfs_node_kind) (AOKFS_GEN_TOOLS_BASE + i);
            return true;
        }
    }
    return false;
}

static const char *aokfs_symlink_target(enum aokfs_node_kind node, size_t *size_out) {
    static const char fixes_debian[] = "devuan";

    switch (node) {
        case aokfs_fixes_debian_link:
            *size_out = sizeof(fixes_debian) - 1;
            return fixes_debian;
        default:
            *size_out = 0;
            return "";
    }
}

static const char *aokfs_inline_file_data(enum aokfs_node_kind node, size_t *size_out) {
    if (aokfs_node_is_gen(node)) {
        const struct aokfs_gen_file *g = aokfs_gen_entry(node);
        *size_out = g->size;
        return g->data;
    }
    static const char readme[] =
        "iSH-AOK support files\n"
        "\n"
        "This is a small support filesystem provided by iSH-AOK.\n"
        "It is mounted at /AOK regardless of the installed Linux rootfs.\n"
        "Most entries are read-only; /AOK/persist is writable and survives root switches.\n";
    static const char version[] = "iSH-AOK\n";
    static const char fixes_devuan_readme[] =
        "pkcsslotd init fix\n"
        "\n"
        "On current Devuan and Debian roots, /usr/sbin/pkcsslotd starts and\n"
        "daemonizes successfully but does not create /var/run/pkcsslotd.pid.\n"
        "\n"
        "The stock /etc/init.d/pkcsslotd script requires that pidfile via\n"
        "start-stop-daemon, so boot-time service management can report failure\n"
        "even while the daemon is already running.\n"
        "\n"
        "Apply the fix with:\n"
        "  sh /AOK/fixes/devuan/fix-pkcsslotd-init.sh\n"
        "\n"
        "The /AOK/fixes/debian entry is a symlink to this same directory.\n";
    static const char fixes_devuan_pkcsslotd_init[] =
        "#!/bin/sh\n"
        "set -e\n"
        "\n"
        "target=/etc/init.d/pkcsslotd\n"
        "backup=/etc/init.d/pkcsslotd.bak\n"
        "\n"
        "if [ ! -x /usr/sbin/pkcsslotd ]; then\n"
        "    echo \"ERROR: /usr/sbin/pkcsslotd is missing\"\n"
        "    exit 1\n"
        "fi\n"
        "\n"
        "if [ -f \"$target\" ] && [ ! -f \"$backup\" ]; then\n"
        "    cp \"$target\" \"$backup\"\n"
        "fi\n"
        "\n"
        "cat >\"$target\" <<'EOF'\n"
        "#!/bin/sh\n"
        "\n"
        "### BEGIN INIT INFO\n"
        "# Provides:             pkcsslotd\n"
        "# Required-Start:       $local_fs $remote_fs\n"
        "# Required-Stop:        $local_fs $remote_fs\n"
        "# Should-Start:\n"
        "# Should-Stop:\n"
        "# Default-Start:        2 3 4 5\n"
        "# Default-Stop:         0 1 6\n"
        "# Short-Description:    starts pkcsslotd\n"
        "# Description:          pkcsslotd belongs to opencryptoki\n"
        "### END INIT INFO\n"
        "\n"
        ". /lib/lsb/init-functions\n"
        "\n"
        "PATH=/sbin:/bin:/usr/sbin:/usr/bin\n"
        "DAEMON=/usr/sbin/pkcsslotd\n"
        "NAME=pkcsslotd\n"
        "DESC=\"PKCS#11 slot daemon\"\n"
        "\n"
        "test -x \"$DAEMON\" || exit 0\n"
        "\n"
        "set -e\n"
        "\n"
        "case \"${1}\" in\n"
        "        start)\n"
        "                echo -n \"Starting $DESC: \"\n"
        "\n"
        "                mkdir -m 0770 -p /var/lock/opencryptoki /var/lock/opencryptoki/icsf /var/lock/opencryptoki/swtok /var/lock/opencryptoki/tpm /var/lock/opencryptoki/lite /var/lock/opencryptoki/ccatok /var/lock/opencryptoki/ep11tok\n"
        "                chown root:pkcs11 /var/lock/opencryptoki /var/lock/opencryptoki/icsf /var/lock/opencryptoki/swtok /var/lock/opencryptoki/tpm /var/lock/opencryptoki/lite /var/lock/opencryptoki/ccatok /var/lock/opencryptoki/ep11tok\n"
        "\n"
        "                start-stop-daemon --start --quiet --oknodo --exec \"$DAEMON\" -- $DAEMON_OPTS\n"
        "                echo \"$NAME.\"\n"
        "                ;;\n"
        "\n"
        "        stop)\n"
        "                echo -n \"Stopping $DESC: \"\n"
        "                start-stop-daemon --stop --oknodo --quiet --exec \"$DAEMON\"\n"
        "                echo \"$NAME.\"\n"
        "                ;;\n"
        "\n"
        "        restart|force-reload)\n"
        "                \"${0}\" stop\n"
        "                sleep 1\n"
        "                \"${0}\" start\n"
        "                ;;\n"
        "\n"
        "        status)\n"
        "                if pidof pkcsslotd >/dev/null 2>&1\n"
        "                then\n"
        "                        echo \"$NAME is running.\"\n"
        "                else\n"
        "                        echo \"$NAME is not running.\"\n"
        "                        exit 1\n"
        "                fi\n"
        "                ;;\n"
        "\n"
        "        *)\n"
        "                N=/etc/init.d/$NAME\n"
        "                echo \"Usage: $N {start|stop|restart|force-reload|status}\" >&2\n"
        "                exit 1\n"
        "                ;;\n"
        "esac\n"
        "\n"
        "exit 0\n"
        "EOF\n"
        "\n"
        "chmod 755 \"$target\"\n"
        "echo \"Installed pkcsslotd init fix at $target\"\n"
        "if [ -f \"$backup\" ]; then\n"
        "    echo \"Backup saved at $backup\"\n"
        "fi\n";
    static const char setup_ish_benchmark[] =
        "#!/bin/sh\n"
        "set -eu\n"
        "\n"
        "archive=${ISH_AOK_BENCHMARK_ARCHIVE:-/AOK/tools/iSH_benchmark.tgz}\n"
        "work_dir=${ISH_AOK_BENCHMARK_DIR:-/tmp/iSH_benchmark}\n"
        "cc=${CC:-gcc}\n"
        "\n"
        "usage() {\n"
        "    cat <<'EOF'\n"
        "Usage: setup-ish-benchmark.sh [make-target]\n"
        "\n"
        "Extract /AOK/tools/iSH_benchmark.tgz into /tmp and compile its benchmarks.\n"
        "\n"
        "Environment:\n"
        "  ISH_AOK_BENCHMARK_ARCHIVE  Archive path. Default: /AOK/tools/iSH_benchmark.tgz\n"
        "  ISH_AOK_BENCHMARK_DIR      Work directory. Default: /tmp/iSH_benchmark\n"
        "  CC                         Compiler passed to make. Default: gcc\n"
        "EOF\n"
        "}\n"
        "\n"
        "case \"${1:-}\" in\n"
        "    -h|--help)\n"
        "        usage\n"
        "        exit 0\n"
        "        ;;\n"
        "esac\n"
        "\n"
        "if [ ! -r \"$archive\" ]; then\n"
        "    echo \"Archive not found: $archive\" >&2\n"
        "    exit 1\n"
        "fi\n"
        "if ! command -v tar >/dev/null 2>&1; then\n"
        "    echo \"tar is required\" >&2\n"
        "    exit 1\n"
        "fi\n"
        "if ! command -v \"$cc\" >/dev/null 2>&1; then\n"
        "    echo \"Compiler not found: $cc\" >&2\n"
        "    exit 1\n"
        "fi\n"
        "\n"
        "rm -rf \"$work_dir\"\n"
        "mkdir -p \"$work_dir\"\n"
        "tar -xzf \"$archive\" -C \"$work_dir\"\n"
        "\n"
        "src_dir=$work_dir/benchmark\n"
        "if [ ! -d \"$src_dir\" ]; then\n"
        "    echo \"Archive did not contain benchmark/\" >&2\n"
        "    exit 1\n"
        "fi\n"
        "\n"
        "make_target=${1:-all}\n"
        "if command -v make >/dev/null 2>&1 && [ -f \"$src_dir/Makefile\" ]; then\n"
        "    make -C \"$src_dir\" CC=\"$cc\" \"$make_target\"\n"
        "elif [ \"$make_target\" = all ]; then\n"
        "    # mirror the archive's Makefile: -O0 pair (historical naive-code\n"
        "    # numbers) plus -O2 pair (distro-shaped code; bmm.c's KEEP barrier\n"
        "    # keeps the optimized loops from folding to constants)\n"
        "    (cd \"$src_dir\" && \"$cc\" -Wall -O0 -o bmm bmm.c -lpthread)\n"
        "    (cd \"$src_dir\" && \"$cc\" -Wall -O0 -o bmt bmt.c -lpthread)\n"
        "    (cd \"$src_dir\" && \"$cc\" -Wall -O2 -o bmm2 bmm.c -lpthread)\n"
        "    (cd \"$src_dir\" && \"$cc\" -Wall -O2 -o bmt2 bmt.c -lpthread)\n"
        "else\n"
        "    echo \"make is required for target: $make_target\" >&2\n"
        "    exit 1\n"
        "fi\n"
        "\n"
        "echo \"Benchmarks extracted and compiled in $src_dir\"\n"
        "echo \"Run $src_dir/bmm or $src_dir/bmt\"\n"
    ;
    switch (node) {
        case aokfs_readme:
            *size_out = sizeof(readme) - 1;
            return readme;
        case aokfs_version:
            *size_out = sizeof(version) - 1;
            return version;
        case aokfs_fixes_devuan_readme:
            *size_out = sizeof(fixes_devuan_readme) - 1;
            return fixes_devuan_readme;
        case aokfs_fixes_devuan_pkcsslotd_init:
            *size_out = sizeof(fixes_devuan_pkcsslotd_init) - 1;
            return fixes_devuan_pkcsslotd_init;
        case aokfs_tools_setup_ish_benchmark:
            *size_out = sizeof(setup_ish_benchmark) - 1;
            return setup_ish_benchmark;
        default:
            *size_out = 0;
            return "";
    }
}

static int aokfs_open_backing_file(struct mount *mount, enum aokfs_node_kind node) {
    if (!aokfs_node_is_bundled_file(node))
        return -1;

    const char *backing_path;
    switch (node) {
        case aokfs_audio_raw:
        case aokfs_audio_wav:
            backing_path = aokfs_node_path(node) + 1;
            break;
        case aokfs_tools_ish_benchmark:
            backing_path = "tools/iSH_benchmark.tgz";
            break;
        default:
            backing_path = aokfs_node_basename(node);
            break;
    }

    char path[MAX_PATH + 1];
    int err = snprintf(path, sizeof(path), "%s/%s", mount->source, backing_path);
    if (err < 0 || err >= (int) sizeof(path))
        return -1;

    int fd = open(path, O_RDONLY);
    if (fd >= 0)
        return fd;

    err = snprintf(path, sizeof(path), "%s/%s", mount->source, aokfs_node_basename(node));
    if (err < 0 || err >= (int) sizeof(path))
        return -1;

    return open(path, O_RDONLY);
}

static int aokfs_inline_stat(enum aokfs_node_kind node, struct statbuf *stat) {
    size_t size = 0;
    if (aokfs_node_is_symlink(node))
        aokfs_symlink_target(node, &size);
    else
        aokfs_inline_file_data(node, &size);
    stat->size = size;
    stat->blksize = 4096;
    stat->blocks = (size + 511) / 512;
    return 0;
}

static int aokfs_host_stat(struct mount *mount, enum aokfs_node_kind node, struct statbuf *stat) {
    int fd = aokfs_open_backing_file(mount, node);
    if (fd < 0)
        return _ENOENT;

    struct stat host_stat;
    if (fstat(fd, &host_stat) < 0) {
        close(fd);
        return errno_map();
    }
    close(fd);

    stat->size = host_stat.st_size;
    stat->blksize = host_stat.st_blksize;
    stat->blocks = host_stat.st_blocks;
    return 0;
}

static int aokfs_stat_common(struct mount *mount, enum aokfs_node_kind node, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->inode = aokfs_node_inode(node);
    stat->mode = aokfs_node_mode(node);
    stat->nlink = aokfs_node_is_dir(node) ? 2 : 1;

    if (aokfs_node_is_dir(node)) {
        stat->blksize = 4096;
        return 0;
    }

    if (aokfs_node_is_bundled_file(node))
        return aokfs_host_stat(mount, node, stat);
    return aokfs_inline_stat(node, stat);
}

static int aokfs_statfs(struct mount *UNUSED(mount), struct statfsbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->type = AOKFS_MAGIC;
    stat->bsize = 4096;
    stat->files = 35;
    stat->ffree = 0;
    stat->namelen = NAME_MAX;
    stat->flags = MS_READONLY_;
    return 0;
}

static int aokfs_stat(struct mount *mount, const char *path, struct statbuf *stat) {
    enum aokfs_node_kind node;
    if (!aokfs_lookup_node(path, &node))
        return _ENOENT;
    return aokfs_stat_common(mount, node, stat);
}

static int aokfs_fstat(struct fd *fd, struct statbuf *stat) {
    return aokfs_stat_common(fd->mount, aokfs_decode_node(fd->fs_data), stat);
}

static int aokfs_getpath(struct fd *fd, char *buf) {
    strcpy(buf, aokfs_node_path(aokfs_decode_node(fd->fs_data)));
    return 0;
}

static ssize_t aokfs_readlink(struct mount *UNUSED(mount), const char *path, char *buf, size_t bufsize) {
    enum aokfs_node_kind node;
    if (!aokfs_lookup_node(path, &node))
        return _ENOENT;
    if (!aokfs_node_is_symlink(node))
        return _EINVAL;

    size_t size = 0;
    const char *target = aokfs_symlink_target(node, &size);
    if (bufsize > size)
        bufsize = size;
    memcpy(buf, target, bufsize);
    return bufsize;
}

static ssize_t aokfs_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    enum aokfs_node_kind node = aokfs_decode_node(fd->fs_data);
    if (aokfs_node_is_dir(node))
        return _EISDIR;

    if (fd->real_fd >= 0) {
        ssize_t res = pread(fd->real_fd, buf, bufsize, off);
        if (res < 0)
            return errno_map();
        return res;
    }

    size_t size = 0;
    const char *data = aokfs_inline_file_data(node, &size);
    if ((size_t) off > size)
        return 0;
    size_t remaining = size - off;
    if (bufsize > remaining)
        bufsize = remaining;
    memcpy(buf, data + off, bufsize);
    return bufsize;
}

static ssize_t aokfs_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t res = aokfs_pread(fd, buf, bufsize, fd->offset);
    if (res > 0)
        fd->offset += res;
    return res;
}

static ssize_t aokfs_write(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t UNUSED(bufsize)) {
    return _EROFS;
}

static ssize_t aokfs_pwrite(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t UNUSED(bufsize), off_t UNUSED(off)) {
    return _EROFS;
}

static off_t_ aokfs_lseek(struct fd *fd, off_t_ off, int whence) {
    enum aokfs_node_kind node = aokfs_decode_node(fd->fs_data);
    if (aokfs_node_is_dir(node))
        return _EINVAL;

    struct statbuf stat;
    int err = aokfs_stat_common(fd->mount, node, &stat);
    if (err < 0)
        return err;
    return generic_seek(fd, off, whence, stat.size);
}

static int aokfs_readdir(struct fd *fd, struct dir_entry *entry) {
    enum aokfs_node_kind node = aokfs_decode_node(fd->fs_data);
    enum aokfs_node_kind child;

    switch (node) {
        case aokfs_root:
            switch (fd->offset++) {
                case 0: child = aokfs_readme; break;
                case 1: child = aokfs_version; break;
                case 2: child = aokfs_persist_dir; break;
                case 3: child = aokfs_roots_dir; break;
                case 4: child = aokfs_fixes_dir; break;
                case 5: child = aokfs_tests_dir; break;
                case 6: child = aokfs_tools_dir; break;
                default: return 0;
            }
            break;
        case aokfs_fixes_dir:
            switch (fd->offset++) {
                case 0: child = aokfs_fixes_devuan_dir; break;
                case 1: child = aokfs_fixes_debian_link; break;
                default: return 0;
            }
            break;
        case aokfs_fixes_devuan_dir:
            switch (fd->offset++) {
                case 0: child = aokfs_fixes_devuan_readme; break;
                case 1: child = aokfs_fixes_devuan_pkcsslotd_init; break;
                default: return 0;
            }
            break;
        case aokfs_tools_dir: {
            // The two bundled tool entries and the ktop subdirectory first,
            // then generated /tools/* files that belong directly to /tools
            // (one path component past the prefix -- ktop's own files live
            // under /tools/ktop/ and are only reachable through that node).
            size_t want = (size_t) fd->offset++;
            if (want == 0) {
                child = aokfs_tools_ish_benchmark;
                break;
            }
            if (want == 1) {
                child = aokfs_tools_setup_ish_benchmark;
                break;
            }
            if (want == 2) {
                child = aokfs_tools_ktop_dir;
                break;
            }
            size_t skip = want - 3;
            const char *prefix = "/tools/";
            size_t plen = strlen(prefix);
            size_t seen = 0;
            bool found = false;
            for (size_t i = 0; i < AOKFS_GEN_FILE_COUNT_tools; i++) {
                const char *p = aokfs_gen_files_tools[i].path;
                if (strncmp(p, prefix, plen) != 0 || strchr(p + plen, '/') != NULL)
                    continue;
                if (seen++ == skip) {
                    child = (enum aokfs_node_kind) (AOKFS_GEN_TOOLS_BASE + i);
                    found = true;
                    break;
                }
            }
            if (!found)
                return 0;
            break;
        }
        case aokfs_tools_ktop_dir: {
            const char *prefix = "/tools/ktop/";
            size_t plen = strlen(prefix);
            size_t want = (size_t) fd->offset++;
            size_t seen = 0;
            bool found = false;
            for (size_t i = 0; i < AOKFS_GEN_FILE_COUNT_tools; i++) {
                const char *p = aokfs_gen_files_tools[i].path;
                if (strncmp(p, prefix, plen) != 0 || strchr(p + plen, '/') != NULL)
                    continue;
                if (seen++ == want) {
                    child = (enum aokfs_node_kind) (AOKFS_GEN_TOOLS_BASE + i);
                    found = true;
                    break;
                }
            }
            if (!found)
                return 0;
            break;
        }
        case aokfs_tests_dir:
        case aokfs_tests_x86_dir:
        case aokfs_tests_arm64_dir: {
            // Generated files that belong DIRECTLY to this directory (one
            // path component past the prefix), then — for /tests itself —
            // the subdirectories.
            const char *prefix = node == aokfs_tests_dir ? "/tests/"
                               : node == aokfs_tests_x86_dir ? "/tests/x86/"
                               : "/tests/arm64/";
            size_t plen = strlen(prefix);
            size_t want = (size_t) fd->offset++;
            size_t seen = 0;
            bool found = false;
            for (size_t i = 0; i < AOKFS_GEN_FILE_COUNT; i++) {
                const char *p = aokfs_gen_files[i].path;
                if (strncmp(p, prefix, plen) != 0 || strchr(p + plen, '/') != NULL)
                    continue;
                if (seen++ == want) {
                    child = (enum aokfs_node_kind) (AOKFS_GEN_BASE + i);
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (node != aokfs_tests_dir)
                    return 0;
                switch (want - seen) {
                    case 0: child = aokfs_tests_audio_dir; break;
                    case 1: child = aokfs_tests_x86_dir; break;
                    case 2: child = aokfs_tests_arm64_dir; break;
                    default: return 0;
                }
            }
            break;
        }
        case aokfs_tests_audio_dir:
            switch (fd->offset++) {
                case 0: child = aokfs_audio_raw; break;
                case 1: child = aokfs_audio_wav; break;
                default: return 0;
            }
            break;
        default:
            return _ENOTDIR;
    }

    entry->inode = aokfs_node_inode(child);
    entry->type = dir_entry_type_for_mode(aokfs_node_mode(child));
    strcpy(entry->name, aokfs_node_basename(child));
    return 1;
}

static int aokfs_close(struct fd *fd) {
    if (fd->real_fd >= 0) {
        if (close(fd->real_fd) < 0)
            return errno_map();
        fd->real_fd = -1;
    }
    return 0;
}

static const struct fd_ops aokfs_fdops = {
    .read = aokfs_read,
    .write = aokfs_write,
    .pread = aokfs_pread,
    .pwrite = aokfs_pwrite,
    .lseek = aokfs_lseek,
    .readdir = aokfs_readdir,
    .close = aokfs_close,
};

static struct fd *aokfs_open(struct mount *mount, const char *path, int UNUSED(flags), int UNUSED(mode)) {
    enum aokfs_node_kind node;
    if (!aokfs_lookup_node(path, &node))
        return ERR_PTR(_ENOENT);

    struct fd *fd = fd_create(&aokfs_fdops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    mount_retain(mount);
    fd->mount = mount;
    fd->type = aokfs_node_mode(node) & S_IFMT;
    fd->fs_data = aokfs_encode_node(node);
    fd->real_fd = -1;

    if (aokfs_node_is_bundled_file(node)) {
        fd->real_fd = aokfs_open_backing_file(mount, node);
        if (fd->real_fd < 0) {
            fd_close(fd);
            return ERR_PTR(_ENOENT);
        }
    }

    return fd;
}

const struct fs_ops aokfs = {
    .name = "aokfs",
    .magic = AOKFS_MAGIC,
    .statfs = aokfs_statfs,
    .open = aokfs_open,
    .stat = aokfs_stat,
    .fstat = aokfs_fstat,
    .readlink = aokfs_readlink,
    .getpath = aokfs_getpath,
};
