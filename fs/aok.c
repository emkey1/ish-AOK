#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "kernel/native.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/hostinfo.h"
#include "fs/fd.h"

#define AOKFS_MAGIC 0x414f4b31

enum aokfs_node_kind {
    aokfs_root = 1,
    aokfs_readme,
    aokfs_version,
    aokfs_persist_dir,
    aokfs_roots_dir,
    aokfs_fakefs_dir,
    aokfs_fixes_dir,
    aokfs_fixes_devuan_dir,
    aokfs_fixes_devuan_readme,
    aokfs_fixes_devuan_pkcsslotd_init,
    aokfs_fixes_debian_link,
    aokfs_fixes_arch_dir,
    aokfs_fixes_arch_readme,
    aokfs_fixes_arch_script,
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
    aokfs_tests_riscv64_dir,
    // ktop's source/build-script subdirectory: same pattern as the arch test
    // dirs above, but rooted at /tools/ktop/ against the *tools* generated
    // table (manifest names like "ktop/ktop.c" become paths under /tools/ktop/).
    aokfs_tools_ktop_dir,
    // Same pattern as aokfs_tools_ktop_dir, rooted at /tools/pixman/ (the
    // pixman-accelerator LD_PRELOAD shim source + build script; manifest
    // names like "pixman/ish_pixman_shim.c" become paths under /tools/pixman/).
    aokfs_tools_pixman_dir,
    // Same again, rooted at /tools/crypto/ (the crypto-accelerator OpenSSL
    // provider source + its installer). A manifest entry alone is not enough
    // for a subdirectory: adding one means adding a node here and listing it
    // in aokfs_node_is_dir, aokfs_node_path, aokfs_lookup_node, and the
    // /tools readdir below, or the files exist in the table but nothing can
    // reach them.
    aokfs_tools_crypto_dir,
    // /docs is flat (no subdirectories) -- same generated-table pattern as
    // /tools, minus the ktop-style subdirectory case.
    aokfs_docs_dir,
    // /native holds the entry points for programs whose implementation is
    // compiled into iSH-AOK itself and runs as host code, never as translated
    // guest code (kernel/native.c). Exec matches on the resolved path, so a
    // symlink from anywhere -- `ln -s /AOK/native/smallclue /usr/local/bin/df`
    // -- dispatches natively while keeping argv[0] as the caller typed it.
    // These files are NOT the program: the bytes served here are the
    // fallback stub described at aokfs_inline_file_data, which only ever runs
    // if native dispatch is unavailable.
    aokfs_native_dir,
    // No constant per program: /native/<name> nodes are AOKFS_NATIVE_BASE plus
    // an index into kernel/native.c's registry. See the base below.
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
#include "aok_generated_docs.inc"
// Native programs are addressed like the generated files above: a base plus an
// index into kernel/native.c's registry, rather than one enum constant per
// program. The registry is already the thing exec dispatches on, so serving
// /AOK/native FROM it means adding a native program cannot leave the filesystem
// and the dispatcher disagreeing about what exists.
#define AOKFS_NATIVE_BASE 0x40000
static bool aokfs_node_is_native(enum aokfs_node_kind node) {
    return (unsigned) node >= AOKFS_NATIVE_BASE &&
        (unsigned) node < AOKFS_NATIVE_BASE + native_program_count();
}
static const struct native_program *aokfs_node_native(enum aokfs_node_kind node) {
    return native_program_at((unsigned) node - AOKFS_NATIVE_BASE);
}

#define AOKFS_GEN_BASE 0x10000
#define AOKFS_GEN_TOOLS_BASE 0x20000
#define AOKFS_GEN_DOCS_BASE 0x30000
static bool aokfs_node_is_gen_tools(enum aokfs_node_kind node) {
    return (unsigned) node >= AOKFS_GEN_TOOLS_BASE &&
        (unsigned) node < AOKFS_GEN_TOOLS_BASE + AOKFS_GEN_FILE_COUNT_tools;
}
static bool aokfs_node_is_gen_docs(enum aokfs_node_kind node) {
    return (unsigned) node >= AOKFS_GEN_DOCS_BASE &&
        (unsigned) node < AOKFS_GEN_DOCS_BASE + AOKFS_GEN_FILE_COUNT_docs;
}
static bool aokfs_node_is_gen(enum aokfs_node_kind node) {
    return ((unsigned) node >= AOKFS_GEN_BASE &&
            (unsigned) node < AOKFS_GEN_BASE + AOKFS_GEN_FILE_COUNT) ||
        aokfs_node_is_gen_tools(node) || aokfs_node_is_gen_docs(node);
}
static const struct aokfs_gen_file *aokfs_gen_entry(enum aokfs_node_kind node) {
    if (aokfs_node_is_gen_tools(node))
        return &aokfs_gen_files_tools[(unsigned) node - AOKFS_GEN_TOOLS_BASE];
    if (aokfs_node_is_gen_docs(node))
        return &aokfs_gen_files_docs[(unsigned) node - AOKFS_GEN_DOCS_BASE];
    return &aokfs_gen_files[(unsigned) node - AOKFS_GEN_BASE];
}

static bool aokfs_node_is_dir(enum aokfs_node_kind node) {
    return node == aokfs_root ||
        node == aokfs_fixes_dir ||
        node == aokfs_persist_dir ||
        node == aokfs_roots_dir ||
        node == aokfs_fakefs_dir ||
        node == aokfs_fixes_devuan_dir ||
        node == aokfs_fixes_arch_dir ||
        node == aokfs_tools_dir ||
        node == aokfs_tests_dir ||
        node == aokfs_tests_audio_dir ||
        node == aokfs_tests_x86_dir ||
        node == aokfs_tests_arm64_dir ||
        node == aokfs_tests_riscv64_dir ||
        node == aokfs_tools_ktop_dir ||
        node == aokfs_tools_pixman_dir ||
        node == aokfs_tools_crypto_dir ||
        node == aokfs_native_dir ||
        node == aokfs_docs_dir;
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
    if (node == aokfs_persist_dir || node == aokfs_roots_dir || node == aokfs_fakefs_dir)
        return S_IFDIR | 0777;
    if (aokfs_node_is_dir(node))
        return S_IFDIR | 0555;
    if (aokfs_node_is_symlink(node))
        return S_IFLNK | 0777;
    if (node == aokfs_tools_setup_ish_benchmark || aokfs_node_is_native(node))
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
        case aokfs_fakefs_dir:
            return "/fakefs";
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
        case aokfs_fixes_arch_dir:
            return "/fixes/arch";
        case aokfs_fixes_arch_readme:
            return "/fixes/arch/README.txt";
        case aokfs_fixes_arch_script:
            return "/fixes/arch/fix-pacman.sh";
        case aokfs_tests_dir:
            return "/tests";
        case aokfs_tests_x86_dir:
            return "/tests/x86";
        case aokfs_tests_arm64_dir:
            return "/tests/arm64";
        case aokfs_tests_riscv64_dir:
            return "/tests/riscv64";
        case aokfs_tools_dir:
            return "/tools";
        case aokfs_tools_ish_benchmark:
            return "/tools/iSH_benchmark.tgz";
        case aokfs_tools_setup_ish_benchmark:
            return "/tools/setup-ish-benchmark.sh";
        case aokfs_tools_ktop_dir:
            return "/tools/ktop";
        case aokfs_tools_pixman_dir:
            return "/tools/pixman";
        case aokfs_tools_crypto_dir:
            return "/tools/crypto";
        case aokfs_tests_audio_dir:
            return "/tests/audio";
        case aokfs_audio_raw:
            return "/tests/audio/test-tone-48k-s16le-stereo.raw";
        case aokfs_audio_wav:
            return "/tests/audio/test-tone-48k-s16le-stereo.wav";
        case aokfs_docs_dir:
            return "/docs";
        case aokfs_native_dir:
            return "/native";
    }
    if (aokfs_node_is_native(node)) {
        // Built per call into a rotating buffer: the callers compare it or copy
        // it immediately, and a native program's name is short and fixed.
        static _Thread_local char path[64];
        snprintf(path, sizeof(path), "/native/%s", aokfs_node_native(node)->name);
        return path;
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
        aokfs_fakefs_dir,
        aokfs_fixes_dir,
        aokfs_fixes_devuan_dir,
        aokfs_fixes_devuan_readme,
        aokfs_fixes_devuan_pkcsslotd_init,
        aokfs_fixes_debian_link,
        aokfs_fixes_arch_dir,
        aokfs_fixes_arch_readme,
        aokfs_fixes_arch_script,
        aokfs_tests_dir,
        aokfs_tests_x86_dir,
        aokfs_tests_arm64_dir,
        aokfs_tests_riscv64_dir,
        aokfs_tools_dir,
        aokfs_tools_ish_benchmark,
        aokfs_tools_setup_ish_benchmark,
        aokfs_tools_ktop_dir,
        aokfs_tools_pixman_dir,
        aokfs_tools_crypto_dir,
        aokfs_tests_audio_dir,
        aokfs_audio_raw,
        aokfs_audio_wav,
        aokfs_docs_dir,
        aokfs_native_dir,
    };

    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        enum aokfs_node_kind node = nodes[i];
        if (strcmp(path, aokfs_node_path(node)) == 0) {
            *node_out = node;
            return true;
        }
    }
    // /native/<name>, straight from the registry.
    for (size_t i = 0; i < native_program_count(); i++) {
        const struct native_program *prog = native_program_at(i);
        char candidate[64];
        snprintf(candidate, sizeof(candidate), "/native/%s", prog->name);
        if (strcmp(path, candidate) == 0) {
            *node_out = (enum aokfs_node_kind) (AOKFS_NATIVE_BASE + i);
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
    // Generated /docs/* files.
    for (size_t i = 0; i < AOKFS_GEN_FILE_COUNT_docs; i++) {
        if (strcmp(path, aokfs_gen_files_docs[i].path) == 0) {
            *node_out = (enum aokfs_node_kind) (AOKFS_GEN_DOCS_BASE + i);
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

// Backing store for /AOK/VERSION, filled on first read (see the comment at its
// declaration below). Kept out here so the once-callback can reach it.
static char aokfs_version_text[128];

static void aokfs_init_version(void) {
    char *build = copyBuildVersion();
    snprintf(aokfs_version_text, sizeof(aokfs_version_text), "iSH-AOK %s%s%s\n",
            build != NULL ? build : "unknown", ISH_BUILD_OPT_SUFFIX, ISH_BUILD_GRET_SUFFIX);
    free(build);
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
        "Most entries are read-only; /AOK/persist and /AOK/fakefs are writable and\n"
        "survive root switches. /AOK/persist is host-backed (visible outside iSH-AOK,\n"
        "but does not preserve Linux ownership or device nodes); /AOK/fakefs preserves\n"
        "full Linux metadata (uid/gid, permissions, device nodes, hardlinks).\n"
        "\n"
        "/AOK/native holds programs implemented inside iSH-AOK itself. Running one\n"
        "executes host code directly instead of translating guest instructions, so it\n"
        "is the same speed on every guest architecture. Link to them from anywhere:\n"
        "  ln -s /AOK/native/smallclue /usr/local/bin/df\n"
        "The link name selects the applet, exactly as on Linux. Use a SYMlink: /AOK is\n"
        "a separate filesystem, so a hard link across it fails with EXDEV.\n";
    // /AOK/version is the documented build identifier, so it carries exactly
    // what `uname -v` reports -- including the build timestamp, because the
    // hand-maintained version number is routinely not bumped between builds
    // and cannot on its own tell you which binary a device is running.
    // Filled once into a static buffer: this function hands back a pointer and
    // a length, and stat and read have to agree on that length.
    static pthread_once_t version_once = PTHREAD_ONCE_INIT;
    static const char fixes_arch_readme[] =
        "Arch Linux ARM under iSH-AOK\n"
        "\n"
        "Three things stop a stock Arch root from installing packages here. None of\n"
        "them is an emulator bug; all three are the root expecting a system service or\n"
        "kernel feature that an AOK guest does not have.\n"
        "\n"
        "1. pacman's sandbox needs Landlock.\n"
        "\n"
        "   pacman 7 confines its download and extraction work with Landlock, the Linux\n"
        "   LSM. AOK's kernel does not implement it and reports ENOSYS, and pacman\n"
        "   treats that as fatal rather than degrading:\n"
        "\n"
        "       error: restricting filesystem access failed because Landlock is not\n"
        "              supported by the kernel!\n"
        "       error: switching to sandbox user 'alpm' failed!\n"
        "\n"
        "   AOK will not pretend to support it. A syscall that claims to have\n"
        "   sandboxed something it did not is worse than one that says it cannot,\n"
        "   because the caller then trusts a confinement that is not there. So the\n"
        "   sandbox is switched off explicitly in pacman.conf, which is what every\n"
        "   kernel without Landlock gets.\n"
        "\n"
        "2. /etc/resolv.conf is a dangling symlink.\n"
        "\n"
        "   The image ships it pointing at /run/systemd/resolve/resolv.conf, which\n"
        "   systemd-resolved would create. Nothing under AOK runs systemd, so the link\n"
        "   never resolves and every mirror lookup fails with \"Could not resolve host\".\n"
        "\n"
        "3. The keyring is empty.\n"
        "\n"
        "   A fresh root has no populated pacman keyring, so signed packages are\n"
        "   refused with \"required key missing from keyring\".\n"
        "\n"
        "Apply all three with:\n"
        "\n"
        "  sh /AOK/fixes/arch/fix-pacman.sh\n"
        "\n"
        "It is safe to re-run: each step checks whether it is already done. The keyring\n"
        "step takes a few minutes and needs no network.\n"
        "\n";

    static const char fixes_arch_script[] =
        "#!/bin/sh\n"
        "# Make a stock Arch Linux ARM root able to install packages under iSH-AOK.\n"
        "# See /AOK/fixes/arch/README.txt for why each step is needed.\n"
        "set -e\n"
        "\n"
        "if [ ! -f /etc/pacman.conf ]; then\n"
        "    echo \"ERROR: /etc/pacman.conf is missing -- this does not look like an Arch root\"\n"
        "    exit 1\n"
        "fi\n"
        "\n"
        "changed=0\n"
        "\n"
        "# 1. Landlock is not implemented by AOK's kernel, and pacman treats its\n"
        "#    absence as fatal rather than degrading. Turn the sandbox off explicitly.\n"
        "for opt in DisableSandboxFilesystem DisableSandboxSyscalls; do\n"
        "    if grep -q \"^${opt}\" /etc/pacman.conf; then\n"
        "        echo \"ok: ${opt} already set\"\n"
        "    else\n"
        "        if grep -q \"^#${opt}\" /etc/pacman.conf; then\n"
        "            sed -i \"s/^#${opt}/${opt}/\" /etc/pacman.conf\n"
        "        else\n"
        "            sed -i \"s/^\\\\[options\\\\]/[options]\\\\n${opt}/\" /etc/pacman.conf\n"
        "        fi\n"
        "        echo \"set: ${opt}\"\n"
        "        changed=1\n"
        "    fi\n"
        "done\n"
        "\n"
        "# 2. The shipped /etc/resolv.conf points at a file systemd-resolved would\n"
        "#    create. Nothing runs systemd here, so it never exists.\n"
        "if [ -e /etc/resolv.conf ] && [ ! -L /etc/resolv.conf ]; then\n"
        "    echo \"ok: /etc/resolv.conf is a real file\"\n"
        "elif [ -L /etc/resolv.conf ] && [ -e /etc/resolv.conf ]; then\n"
        "    echo \"ok: /etc/resolv.conf symlink resolves\"\n"
        "else\n"
        "    rm -f /etc/resolv.conf\n"
        "    printf 'nameserver 1.1.1.1\\nnameserver 8.8.8.8\\n' > /etc/resolv.conf\n"
        "    echo \"set: /etc/resolv.conf replaced (was a dangling symlink)\"\n"
        "    changed=1\n"
        "fi\n"
        "\n"
        "# 3. A fresh root has no populated keyring, so signed packages are refused.\n"
        "if [ -s /etc/pacman.d/gnupg/trustdb.gpg ]; then\n"
        "    echo \"ok: pacman keyring already initialised\"\n"
        "else\n"
        "    echo \"initialising the pacman keyring (a few minutes, no network needed)...\"\n"
        "    pacman-key --init\n"
        "    if pacman-key --populate archlinuxarm 2>/dev/null; then\n"
        "        echo \"set: keyring populated from archlinuxarm\"\n"
        "    else\n"
        "        pacman-key --populate\n"
        "        echo \"set: keyring populated\"\n"
        "    fi\n"
        "    changed=1\n"
        "fi\n"
        "\n"
        "if [ \"$changed\" = \"0\" ]; then\n"
        "    echo\n"
        "    echo \"Nothing to do -- this root is already set up.\"\n"
        "else\n"
        "    echo\n"
        "    echo \"Done. Try:  pacman -Sy\"\n"
        "fi\n"
        "\n";

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
    // Fallback stub for /AOK/native/*. These paths are dispatched by exec
    // straight into compiled-in host code (kernel/native.c) and their contents
    // are never read on that path, so what lives here only matters when native
    // dispatch does NOT happen -- the build lacks the program, or the file was
    // copied somewhere the dispatcher no longer recognizes. A shebang script
    // rather than an ELF stub on purpose: it is guest-ABI-neutral (native
    // dispatch serves i386/amd64/arm64/riscv64 guests from one implementation,
    // and a real ELF fallback would need one build per guest arch), and it
    // fails loudly with a diagnostic instead of confusingly with ENOEXEC.
    static const char native_stub[] =
        "#!/bin/sh\n"
        "# Placeholder for a program implemented natively inside iSH-AOK.\n"
        "# Reaching this text means native dispatch did not happen for this\n"
        "# path -- see /AOK/README.txt.\n"
        "echo \"${0##*/}: native dispatch unavailable in this build\" >&2\n"
        "exit 127\n";
    if (aokfs_node_is_native(node)) {
        *size_out = sizeof(native_stub) - 1;
        return native_stub;
    }
    switch (node) {
        case aokfs_readme:
            *size_out = sizeof(readme) - 1;
            return readme;

        case aokfs_version:
            pthread_once(&version_once, aokfs_init_version);
            *size_out = strlen(aokfs_version_text);
            return aokfs_version_text;
        case aokfs_fixes_devuan_readme:
            *size_out = sizeof(fixes_devuan_readme) - 1;
            return fixes_devuan_readme;
        case aokfs_fixes_devuan_pkcsslotd_init:
            *size_out = sizeof(fixes_devuan_pkcsslotd_init) - 1;
            return fixes_devuan_pkcsslotd_init;
        case aokfs_fixes_arch_readme:
            *size_out = sizeof(fixes_arch_readme) - 1;
            return fixes_arch_readme;
        case aokfs_fixes_arch_script:
            *size_out = sizeof(fixes_arch_script) - 1;
            return fixes_arch_script;
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

// Timestamp for everything aokfs synthesizes. Its content -- the inline files,
// the generated /tests, /tools and /docs tables, and the directories holding
// them -- is compiled into the binary, so the build is genuinely when it last
// changed, and it is the same stamp uname -v and /AOK/VERSION report (see
// kernel/hostinfo.h), which is what lets `ls -l /AOK` corroborate them.
//
// Not doing this at all is what the fs used to do: the whole tree reported
// mtime 0, and `ls -alt /AOK/docs` printed "Dec 31 1969" for every entry.
static time_t aokfs_build_time;
static void aokfs_init_build_time(void) {
    aokfs_build_time = buildTimestamp();
    // Only if the host won't stat its own executable, which shouldn't happen.
    // Anything plausible beats falling back to 0, which is the bug being fixed.
    if (aokfs_build_time == 0)
        aokfs_build_time = time(NULL);
}

static time_t aokfs_default_time(void) {
    static pthread_once_t build_time_once = PTHREAD_ONCE_INIT;
    pthread_once(&build_time_once, aokfs_init_build_time);
    return aokfs_build_time;
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
    // These three are real files in the app bundle, so they have real times --
    // report them instead of the build stamp the caller filled in.
    stat->atime = host_stat.st_atime;
    stat->mtime = host_stat.st_mtime;
    stat->ctime = host_stat.st_ctime;
#if __APPLE__
#define TIMESPEC(x) st_##x##timespec
#elif __linux__
#define TIMESPEC(x) st_##x##tim
#endif
    stat->atime_nsec = host_stat.TIMESPEC(a).tv_nsec;
    stat->mtime_nsec = host_stat.TIMESPEC(m).tv_nsec;
    stat->ctime_nsec = host_stat.TIMESPEC(c).tv_nsec;
#undef TIMESPEC
    return 0;
}

static int aokfs_stat_common(struct mount *mount, enum aokfs_node_kind node, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->inode = aokfs_node_inode(node);
    stat->mode = aokfs_node_mode(node);
    stat->nlink = aokfs_node_is_dir(node) ? 2 : 1;
    // Before the directory return below, so directories, inline files,
    // generated files and the symlink all get it; aokfs_host_stat overwrites
    // it for the three nodes that have a real file behind them.
    stat->atime = stat->mtime = stat->ctime = aokfs_default_time();

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

const char *aokfs_native_program_name(struct fd *fd) {
    if (fd == NULL || fd->mount == NULL || fd->mount->fs != &aokfs)
        return NULL;
    enum aokfs_node_kind node = aokfs_decode_node(fd->fs_data);
    return aokfs_node_is_native(node) ? aokfs_node_native(node)->name : NULL;
}

static int aokfs_getpath(struct fd *fd, char *buf) {
    const char *path = aokfs_node_path(aokfs_decode_node(fd->fs_data));
    strncpy(buf, path, MAX_PATH - 1);
    buf[MAX_PATH - 1] = '\0';
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
                case 4: child = aokfs_fakefs_dir; break;
                case 5: child = aokfs_fixes_dir; break;
                case 6: child = aokfs_tests_dir; break;
                case 7: child = aokfs_tools_dir; break;
                case 8: child = aokfs_docs_dir; break;
                case 9: child = aokfs_native_dir; break;
                default: return 0;
            }
            break;
        case aokfs_native_dir: {
            size_t i = (size_t) fd->offset++;
            if (i >= native_program_count())
                return 0;
            child = (enum aokfs_node_kind) (AOKFS_NATIVE_BASE + i);
            break;
        }
        case aokfs_fixes_dir:
            switch (fd->offset++) {
                case 0: child = aokfs_fixes_devuan_dir; break;
                case 1: child = aokfs_fixes_debian_link; break;
                case 2: child = aokfs_fixes_arch_dir; break;
                default: return 0;
            }
            break;
        case aokfs_fixes_arch_dir:
            switch (fd->offset++) {
                case 0: child = aokfs_fixes_arch_readme; break;
                case 1: child = aokfs_fixes_arch_script; break;
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
            // The two bundled tool entries and the ktop/pixman/crypto
            // subdirectories first, then generated /tools/* files that belong
            // directly to /tools (one path component past the prefix -- the
            // subdirectories' own files live under their /tools/<name>/ node
            // and are only reachable through it).
            static const enum aokfs_node_kind tools_fixed[] = {
                aokfs_tools_ish_benchmark,
                aokfs_tools_setup_ish_benchmark,
                aokfs_tools_ktop_dir,
                aokfs_tools_pixman_dir,
                aokfs_tools_crypto_dir,
            };
            size_t nfixed = sizeof(tools_fixed) / sizeof(tools_fixed[0]);
            size_t want = (size_t) fd->offset++;
            if (want < nfixed) {
                child = tools_fixed[want];
                break;
            }
            size_t skip = want - nfixed;
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
        case aokfs_tools_ktop_dir:
        case aokfs_tools_pixman_dir:
        case aokfs_tools_crypto_dir: {
            // One /tools/<name>/ subdirectory: the same generated-table scan
            // /tools itself does, but rooted at this node's own path, so a new
            // subdirectory needs no code here beyond its case label.
            const char *base = aokfs_node_path(node);
            size_t blen = strlen(base);
            size_t want = (size_t) fd->offset++;
            size_t seen = 0;
            bool found = false;
            for (size_t i = 0; i < AOKFS_GEN_FILE_COUNT_tools; i++) {
                const char *p = aokfs_gen_files_tools[i].path;
                if (strncmp(p, base, blen) != 0 || p[blen] != '/' ||
                        strchr(p + blen + 1, '/') != NULL)
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
        case aokfs_docs_dir: {
            // Flat: no subdirectories under /docs, so this is just the
            // generated-table scan, no prefix-skipping ktop-style logic.
            size_t want = (size_t) fd->offset++;
            if (want >= AOKFS_GEN_FILE_COUNT_docs)
                return 0;
            child = (enum aokfs_node_kind) (AOKFS_GEN_DOCS_BASE + want);
            break;
        }
        case aokfs_tests_dir:
        case aokfs_tests_x86_dir:
        case aokfs_tests_arm64_dir:
        case aokfs_tests_riscv64_dir: {
            // Generated files that belong DIRECTLY to this directory (one
            // path component past the prefix), then — for /tests itself —
            // the subdirectories.
            const char *prefix = node == aokfs_tests_dir ? "/tests/"
                               : node == aokfs_tests_x86_dir ? "/tests/x86/"
                               : node == aokfs_tests_arm64_dir ? "/tests/arm64/"
                               : "/tests/riscv64/";
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
                    case 3: child = aokfs_tests_riscv64_dir; break;
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
    strncpy(entry->name, aokfs_node_basename(child), sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
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
