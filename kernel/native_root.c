// The root filesystem of native mode.
//
// Native mode (docs/native_mode_plan.md) boots a root with no Linux
// distribution in it. Its /bin and /usr/bin are symlinks into /AOK/native, the
// login shell is native zsh, and SmallCLUE's init is pid 1. The root itself is
// an ordinary, persistent fakefs -- home directories, ~/.ssh and dotfiles have
// to survive a relaunch -- and this file is what turns an empty one into
// something that boots, and keeps it in step with the build.
//
// It runs at EVERY boot of a native root, not once at creation, because what
// it links is a property of the build: an app update that adds an applet
// should find it on PATH at the next launch, and one that drops a program
// should not leave a link to nothing. Three kinds of path, treated three ways:
//
//   - the user's files (/etc/passwd, /etc/profile, /etc/rc, ...): written only
//     when missing, so after the first boot they are the user's to edit;
//   - links into /AOK/native: made when missing, re-pointed when this file
//     wants a different target, and removed when the build no longer has the
//     program -- but only links that point into /AOK/native, so a file or a
//     link the user put at one of these paths is never touched (the rule
//     native-links.sh follows on distro roots);
//   - AOK's own records (/etc/os-release, /etc/aok-native.links): rewritten.
//
// Everything goes through the guest VFS (generic_*), as the running task, so
// the CLI and the app provision identically.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fs/fd.h"
#include "fs/path.h"
#include "fs/real.h"
#include "kernel/calls.h"
#include "kernel/abi.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/native.h"
#include "kernel/native_root.h"
#include "kernel/random.h"
#include "kernel/sha_crypt.h"
#include "kernel/task.h"
#include "deps/smallclue/src/smallclue.h"

#define NATIVE_DIR "/AOK/native/"
#define APPLETS_CONF "/AOK/tools/native-applets.conf"
#define LINK_RECORD "/etc/aok-native.links"
#define USER_SKIPPED "/etc/aok-native-user-skipped"

// ------------------------------------------------------------- small helpers

static bool path_exists(const char *path) {
    struct statbuf stat;
    return generic_statat(AT_PWD, path, &stat, AT_SYMLINK_NOFOLLOW_) == 0;
}

static void ensure_dir(const char *path, mode_t_ mode) {
    if (path_exists(path))
        return;
    if (generic_mkdirat(AT_PWD, path, mode) == 0)
        // mkdir applies the umask; the mode is the point for /tmp and /root.
        generic_setattrat(AT_PWD, path, make_attr(mode, mode), false);
}

// Read a whole (small) guest file into a NUL-terminated heap buffer.
static char *read_file(const char *path, size_t *len_out) {
    struct fd *fd = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(fd))
        return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    while (buf != NULL) {
        if (len + 1 >= cap) {
            char *bigger = cap < (1 << 20) ? realloc(buf, cap * 2) : NULL;
            if (bigger == NULL) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = bigger;
            cap *= 2;
        }
        ssize_t n = fd->ops->read(fd, buf + len, cap - 1 - len);
        if (n <= 0)
            break;
        len += (size_t) n;
    }
    fd_close(fd);
    if (buf == NULL)
        return NULL;
    buf[len] = '\0';
    if (len_out != NULL)
        *len_out = len;
    return buf;
}

// Write CONTENT to PATH with MODE. With only_if_missing, an existing path (of
// any kind) is left as it is. Otherwise the file is replaced through a rename,
// so a reader never sees half of it.
static int write_file(const char *path, const char *content, mode_t_ mode,
                      bool only_if_missing) {
    if (only_if_missing && path_exists(path))
        return 0;
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.aok-new", path);
    generic_unlinkat(AT_PWD, tmp);
    struct fd *fd = generic_open(tmp, O_WRONLY_ | O_CREAT_ | O_EXCL_ | O_NOFOLLOW_, mode);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);
    size_t len = strlen(content), done = 0;
    int err = 0;
    while (done < len) {
        ssize_t n = fd->ops->write(fd, content + done, len - done);
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

static bool word_in(const char *word, const char *list) {
    size_t n = strlen(word);
    for (const char *p = list; p != NULL && *p != '\0'; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        const char *end = p;
        while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\n')
            end++;
        if ((size_t) (end - p) == n && strncmp(p, word, n) == 0)
            return true;
        p = end;
    }
    return false;
}

// ------------------------------------------------- the applet exclusion lists

// The value of NAME="..." in native-applets.conf, which may span lines. Only
// plain assignments are understood; that file says so to whoever edits it.
static char *conf_value(const char *conf, const char *name) {
    size_t n = strlen(name);
    for (const char *line = conf; line != NULL && *line != '\0'; ) {
        if (strncmp(line, name, n) == 0 && line[n] == '=' && line[n + 1] == '"') {
            const char *start = line + n + 2;
            const char *end = strchr(start, '"');
            if (end == NULL)
                return NULL;
            return strndup(start, (size_t) (end - start));
        }
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }
    return NULL;
}

// ------------------------------------------------------------------- links

struct link_set {
    char **paths;
    size_t count, cap;
};

static void link_set_add(struct link_set *set, const char *path) {
    if (set->count == set->cap) {
        size_t cap = set->cap ? set->cap * 2 : 256;
        char **paths = realloc(set->paths, cap * sizeof(*paths));
        if (paths == NULL)
            return;
        set->paths = paths;
        set->cap = cap;
    }
    char *copy = strdup(path);
    if (copy != NULL)
        set->paths[set->count++] = copy;
}

static bool link_set_has(const struct link_set *set, const char *path) {
    for (size_t i = 0; i < set->count; i++)
        if (strcmp(set->paths[i], path) == 0)
            return true;
    return false;
}

static void link_set_free(struct link_set *set) {
    for (size_t i = 0; i < set->count; i++)
        free(set->paths[i]);
    free(set->paths);
    *set = (struct link_set) {0};
}

// Whether PATH is a symlink this file may manage: one pointing into
// /AOK/native. TARGET_OUT gets its text when it is a symlink at all.
static bool is_our_link(const char *path, char *target_out, size_t size) {
    ssize_t n = generic_readlinkat(AT_PWD, path, target_out, size - 1);
    if (n < 0)
        return false;
    target_out[n] = '\0';
    return strncmp(target_out, NATIVE_DIR, strlen(NATIVE_DIR)) == 0;
}

// Make LINK point at TARGET, unless something that is not ours is there.
static void place_link(struct link_set *made, const char *target, const char *link) {
    char current_target[MAX_PATH];
    if (path_exists(link)) {
        if (!is_our_link(link, current_target, sizeof(current_target)))
            return;   // the user's file or link: never ours to replace
        if (strcmp(current_target, target) != 0) {
            generic_unlinkat(AT_PWD, link);
            generic_symlinkat(target, AT_PWD, link);
        }
    } else {
        generic_symlinkat(target, AT_PWD, link);
    }
    link_set_add(made, link);
}

static bool native_program_built(const char *name) {
    return native_program_lookup(name) != NULL;
}

static void link_everything(struct link_set *made) {
    size_t conf_len = 0;
    char *conf = read_file(APPLETS_CONF, &conf_len);
    char *broken = conf ? conf_value(conf, "BROKEN") : NULL;
    char *not_commands = conf ? conf_value(conf, "NOT_COMMANDS") : NULL;
    free(conf);
    // Without the file, still never link the multicall binary's own names.
    if (not_commands == NULL)
        not_commands = strdup("smallclue smallclue-help licenses");

    // The shells, where scripts and the kernel's ENOEXEC fallback look.
    const char *shell_links[][2] = {
        {NATIVE_DIR "sh",   "/bin/sh"},
        {NATIVE_DIR "dash", "/bin/dash"},
        {NATIVE_DIR "zsh",  "/bin/zsh"},
        {NATIVE_DIR "bash", "/bin/bash"},
    };
    for (size_t i = 0; i < sizeof(shell_links) / sizeof(shell_links[0]); i++)
        if (native_program_built(shell_links[i][0] + strlen(NATIVE_DIR)))
            place_link(made, shell_links[i][0], shell_links[i][1]);

    // The standalone programs, each to its own file. su, sudo and passwd are
    // the setuid-root ones, and these links are what make a bare `sudo` (and
    // the app's /bin/su callers) reach them rather than SmallCLUE's applets.
    for (size_t i = 0; i < native_program_count(); i++) {
        const struct native_program *prog = native_program_at(i);
        if (prog == NULL)
            continue;
        const char *name = prog->name;
        if (strcmp(name, "smallclue") == 0 || strcmp(name, "rust-probe") == 0 ||
            strcmp(name, "zsh-multio") == 0)
            continue;
        char target[MAX_PATH], link[MAX_PATH];
        snprintf(target, sizeof(target), NATIVE_DIR "%s", name);
        snprintf(link, sizeof(link), "/usr/bin/%s", name);
        place_link(made, target, link);
        if (strcmp(name, "su") == 0)
            place_link(made, target, "/bin/su");
    }

    // SmallCLUE's applets. The ones a distro root keeps off PATH because a
    // distro already has them (native-applets.conf's NO_SHADOW) are linked
    // here, since there is nothing to shadow -- the system ones in /sbin.
    size_t count = 0;
    const SmallclueApplet *applets = smallclueGetApplets(&count);
    for (size_t i = 0; i < count; i++) {
        const SmallclueApplet *applet = &applets[i];
        const char *name = applet->name;
        if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL)
            continue;
        if (applet->available != NULL && !applet->available())
            continue;
        if (word_in(name, broken) || word_in(name, not_commands))
            continue;
        if (native_program_built(name))
            continue;   // a standalone program of the same name won above
        char link[MAX_PATH];
        if (strcmp(name, "init") == 0 || strcmp(name, "halt") == 0 ||
            strcmp(name, "reboot") == 0 || strcmp(name, "poweroff") == 0 ||
            strcmp(name, "runit") == 0 || strcmp(name, "mdev") == 0)
            snprintf(link, sizeof(link), "/sbin/%s", name);
        else if (strcmp(name, "login") == 0)
            snprintf(link, sizeof(link), "/bin/%s", name);
        else
            snprintf(link, sizeof(link), "/usr/bin/%s", name);
        place_link(made, NATIVE_DIR "smallclue", link);
    }
    free(broken);
    free(not_commands);
}

// Remove links an earlier boot made that this one did not: the program left
// the build, or moved. Only while they still point into /AOK/native.
static void unlink_stale(const struct link_set *made, const char *previous) {
    for (const char *line = previous; line != NULL && *line != '\0'; ) {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t) (end - line) : strlen(line);
        if (len > 0 && len < MAX_PATH && line[0] == '/') {
            char path[MAX_PATH], target[MAX_PATH];
            memcpy(path, line, len);
            path[len] = '\0';
            if (!link_set_has(made, path) && is_our_link(path, target, sizeof(target)))
                generic_unlinkat(AT_PWD, path);
        }
        line = end != NULL ? end + 1 : NULL;
    }
}

static void record_links(const struct link_set *made) {
    size_t size = 128;
    for (size_t i = 0; i < made->count; i++)
        size += strlen(made->paths[i]) + 1;
    char *text = malloc(size);
    if (text == NULL)
        return;
    size_t at = (size_t) snprintf(text, size,
        "# Links iSH-AOK made into /AOK/native at the last boot. Rewritten at\n"
        "# every boot; a path that drops out of it is removed.\n");
    for (size_t i = 0; i < made->count && at < size; i++)
        at += (size_t) snprintf(text + at, size - at, "%s\n", made->paths[i]);
    write_file(LINK_RECORD, text, 0644, false);
    free(text);
}

// ------------------------------------------------------------------- /etc

static const char *login_shell(void) {
    if (native_program_built("zsh"))
        return NATIVE_DIR "zsh";
    return NATIVE_DIR "sh";
}

static long days_since_epoch(void) {
    return (long) (time(NULL) / 86400);
}

static void write_etc(void) {
    char buf[1024];

    snprintf(buf, sizeof(buf), "root:x:0:0:root:/root:%s\n", login_shell());
    write_file("/etc/passwd", buf, 0644, true);
    write_file("/etc/group",
               "root:x:0:\n"
               "wheel:x:10:root\n"
               "sudo:x:27:\n"
               "users:x:100:\n", 0644, true);
    // Root has no password, and none is needed: the Session Shell opens it
    // with `login -f root`, which does not authenticate. `*` is a field no
    // password hashes to, so su to root cannot be talked into it either.
    snprintf(buf, sizeof(buf), "root:*:%ld:0:99999:7:::\n", days_since_epoch());
    write_file("/etc/shadow", buf, 0600, true);
    write_file("/etc/sudoers",
               "# sudoers for an iSH-AOK native root. Read by /AOK/native/sudo.\n"
               "root    ALL=(ALL:ALL) ALL\n"
               "%sudo   ALL=(ALL:ALL) ALL\n"
               "%wheel  ALL=(ALL:ALL) ALL\n", 0440, true);
    write_file("/etc/shells",
               "/bin/sh\n/bin/dash\n/bin/zsh\n"
               NATIVE_DIR "sh\n" NATIVE_DIR "dash\n" NATIVE_DIR "zsh\n", 0644, true);
    write_file("/etc/hosts",
               "127.0.0.1\tlocalhost\n"
               "::1\tlocalhost ip6-localhost ip6-loopback\n", 0644, true);
    write_file("/etc/profile",
               "# /etc/profile -- an iSH-AOK native root. Yours to edit: iSH-AOK writes\n"
               "# it only when it is missing.\n"
               "export PATH=/AOK/persist/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n"
               "export PAGER=less\n"
               "export EDITOR=vi\n"
               "umask 022\n"
               "for f in /etc/profile.d/*.sh; do\n"
               "    [ -r \"$f\" ] && . \"$f\"\n"
               "done\n"
               "unset f\n", 0644, true);
    // zsh reads /etc/zprofile, not /etc/profile; this is Debian's answer.
    write_file("/etc/zprofile",
               "# Run /etc/profile in sh emulation, as Debian's zsh does.\n"
               "emulate sh -c '. /etc/profile'\n", 0644, true);
    write_file("/etc/zshrc",
               "# /etc/zshrc -- an iSH-AOK native root. Yours to edit: iSH-AOK writes it\n"
               "# only when it is missing.\n"
               "PS1='%n@%m:%~%# '\n"
               "bindkey -e\n"
               "HISTFILE=~/.zsh_history\n"
               "HISTSIZE=5000\n"
               "SAVEHIST=5000\n"
               "setopt append_history hist_ignore_dups\n"
               "if (( ${#fpath} )); then\n"
               "    autoload -Uz compinit && compinit -u -d ~/.zcompdump\n"
               "fi\n", 0644, true);
    write_file("/etc/os-release",
               "NAME=\"iSH-AOK Native\"\n"
               "PRETTY_NAME=\"iSH-AOK Native (no distribution)\"\n"
               "ID=aok-native\n"
               "HOME_URL=\"https://github.com/emkey1/ish-AOK\"\n", 0644, false);

    // The rc system: /etc/rc runs at boot under SmallCLUE's init, and
    // /etc/rc.shutdown when the system halts. Both are plain scripts so what
    // happens at boot can be read and changed; see docs/native_mode_plan.md.
    write_file("/etc/rc",
               "#!/bin/sh\n"
               "# /etc/rc -- run once by init at boot. Yours to edit: iSH-AOK writes it\n"
               "# only when it is missing.\n"
               "#\n"
               "# Runs every executable /etc/rc.d/S??* with `start`, in order; starts\n"
               "# runit for /etc/service if anything is there; then /etc/rc.local.\n"
               "PATH=/usr/sbin:/usr/bin:/sbin:/bin\n"
               "export PATH\n"
               "for s in /etc/rc.d/S??*; do\n"
               "    [ -x \"$s\" ] || continue\n"
               "    \"$s\" start || echo \"rc: $s start failed ($?)\" > /dev/kmsg\n"
               "done\n"
               "for d in /etc/service/*/; do\n"
               "    [ -x \"${d}run\" ] && { runit /etc/service & break; }\n"
               "done\n"
               "[ -x /etc/rc.local ] && /etc/rc.local\n"
               "exit 0\n", 0755, true);
    write_file("/etc/rc.shutdown",
               "#!/bin/sh\n"
               "# /etc/rc.shutdown -- run by init when the system halts. Yours to edit.\n"
               "PATH=/usr/sbin:/usr/bin:/sbin:/bin\n"
               "export PATH\n"
               "command -v sv >/dev/null && sv stop /etc/service/* 2>/dev/null\n"
               "for s in $(ls -r /etc/rc.d/S??* 2>/dev/null); do\n"
               "    [ -x \"$s\" ] && \"$s\" stop\n"
               "done\n"
               "exit 0\n", 0755, true);
    write_file("/etc/rc.d/S50example.disabled",
               "#!/bin/sh\n"
               "# An example boot script. Copy it to a name without .disabled, such as\n"
               "# /etc/rc.d/S50mything, and make it executable: /etc/rc runs it with\n"
               "# `start` at boot and /etc/rc.shutdown with `stop` at halt, in order of\n"
               "# name. A long-running daemon belongs in /etc/service/<name>/run\n"
               "# instead, where runit restarts it if it dies.\n"
               "case \"$1\" in\n"
               "    start) echo \"example: started\" ;;\n"
               "    stop)  echo \"example: stopped\" ;;\n"
               "esac\n", 0644, true);

    // The terminfo database the app carries, where ncurses-built programs and
    // the shim both look. A real directory here (a user installed one) wins.
    if (!path_exists("/usr/share/terminfo"))
        generic_symlinkat(NATIVE_DIR "libs/terminfo", AT_PWD, "/usr/share/terminfo");

    // /etc/mtab, as distros have it.
    if (!path_exists("/etc/mtab"))
        generic_symlinkat("/proc/self/mounts", AT_PWD, "/etc/mtab");
}

void native_root_adopt_abi(struct task *task) {
    task->abi = GUEST_ABI_ARM64;
}

int native_root_provision(void) {
    static const struct {
        const char *path;
        mode_t_ mode;
    } dirs[] = {
        {"/bin", 0755}, {"/sbin", 0755}, {"/usr", 0755}, {"/usr/bin", 0755},
        {"/usr/sbin", 0755}, {"/usr/local", 0755}, {"/usr/local/bin", 0755},
        {"/usr/local/sbin", 0755}, {"/usr/share", 0755}, {"/etc", 0755},
        {"/etc/profile.d", 0755}, {"/etc/rc.d", 0755}, {"/etc/service", 0755},
        {"/etc/skel", 0755}, {"/root", 0700}, {"/home", 0755},
        {"/tmp", 01777}, {"/var", 0755}, {"/var/tmp", 01777},
        {"/var/log", 0755}, {"/run", 0755}, {"/dev", 0755}, {"/proc", 0555},
        {"/sys", 0555}, {"/mnt", 0755},
    };
    struct statbuf stat;
    if (generic_statat(AT_PWD, "/", &stat, 0) < 0 || !S_ISDIR(stat.mode))
        return _ENOENT;
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        ensure_dir(dirs[i].path, dirs[i].mode);
    if (generic_statat(AT_PWD, NATIVE_DIR "smallclue", &stat, 0) < 0)
        return _ENOENT;   // /AOK is not mounted yet: nothing to link to

    // An empty fakefs records its creator as the owner of /, which is the
    // HOST user (501 on a Mac): a root nobody in /etc/passwd owns.
    if (generic_statat(AT_PWD, "/", &stat, 0) == 0 && (stat.uid != 0 || stat.gid != 0)) {
        generic_setattrat(AT_PWD, "/", make_attr(uid, 0), false);
        generic_setattrat(AT_PWD, "/", make_attr(gid, 0), false);
    }

    write_etc();

    char *previous = read_file(LINK_RECORD, NULL);
    struct link_set made = {0};
    link_everything(&made);
    if (previous != NULL)
        unlink_stale(&made, previous);
    record_links(&made);
    link_set_free(&made);
    free(previous);
    return 0;
}

int native_root_mount_zoneinfo(void) {
    // Already there: a mount from earlier in this boot, or zones the user
    // installed. Either way nothing to add.
    if (path_exists("/usr/share/zoneinfo/UTC"))
        return 0;
    char host[MAX_PATH];
    if (realpath("/usr/share/zoneinfo", host) == NULL)
        return _ENOENT;
    ensure_dir("/usr/share/zoneinfo", 0755);
    return do_mount(&realfs, host, "/usr/share/zoneinfo", "", MS_READONLY_);
}

// ------------------------------------------------------------- the account

// The line in an /etc/passwd- or /etc/group-shaped file whose first field is
// NAME, or whose third field is ID (when ID >= 0).
static bool db_has(const char *file, const char *name, long id) {
    char *text = read_file(file, NULL);
    if (text == NULL)
        return false;
    bool found = false;
    for (char *line = text; line != NULL && *line != '\0' && !found; ) {
        char *end = strchr(line, '\n');
        if (end != NULL)
            *end = '\0';
        char *f1 = strchr(line, ':');
        if (f1 != NULL) {
            if (name != NULL && (size_t) (f1 - line) == strlen(name) &&
                strncmp(line, name, (size_t) (f1 - line)) == 0)
                found = true;
            char *f2 = strchr(f1 + 1, ':');
            if (!found && id >= 0 && f2 != NULL && strtol(f2 + 1, NULL, 10) == id &&
                f2[1] >= '0' && f2[1] <= '9')
                found = true;
        }
        line = end != NULL ? end + 1 : NULL;
    }
    free(text);
    return found;
}

bool native_root_has_default_user(void) {
    return db_has("/etc/passwd", NULL, NATIVE_ROOT_USER_UID);
}

bool native_root_default_user_skipped(void) {
    return path_exists(USER_SKIPPED);
}

int native_root_skip_default_user(void) {
    return write_file(USER_SKIPPED,
                      "# The first-start prompt for an everyday account was skipped.\n"
                      "# Remove this file to be asked again at the next launch.\n",
                      0644, false);
}

bool native_root_user_name_valid(const char *name) {
    if (name == NULL || name[0] == '\0' || strlen(name) > 32)
        return false;
    if (!(name[0] == '_' || (name[0] >= 'a' && name[0] <= 'z')))
        return false;
    for (const char *p = name + 1; *p != '\0'; p++)
        if (!(*p == '_' || *p == '-' || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9')))
            return false;
    return !db_has("/etc/passwd", name, -1) && !db_has("/etc/group", name, -1);
}

// Append LINE to FILE, keeping its mode.
static int append_line(const char *file, const char *line, mode_t_ mode) {
    size_t len = 0;
    char *text = read_file(file, &len);
    size_t size = len + strlen(line) + 2;
    char *joined = malloc(size);
    if (joined == NULL) {
        free(text);
        return _ENOMEM;
    }
    snprintf(joined, size, "%s%s%s", text ? text : "",
             (len > 0 && text[len - 1] != '\n') ? "\n" : "", line);
    free(text);
    int err = write_file(file, joined, mode, false);
    free(joined);
    return err;
}

// Add NAME to the member list of GROUP in /etc/group.
static int add_to_group(const char *group, const char *name) {
    char *text = read_file("/etc/group", NULL);
    if (text == NULL)
        return _ENOENT;
    size_t size = strlen(text) + strlen(name) + 64;
    char *out = malloc(size);
    if (out == NULL) {
        free(text);
        return _ENOMEM;
    }
    size_t at = 0, glen = strlen(group);
    bool done = false;
    for (char *line = text; line != NULL && *line != '\0'; ) {
        char *end = strchr(line, '\n');
        if (end != NULL)
            *end = '\0';
        size_t len = strlen(line);
        memcpy(out + at, line, len);
        at += len;
        if (!done && strncmp(line, group, glen) == 0 && line[glen] == ':') {
            bool empty = line[len - 1] == ':';
            at += (size_t) snprintf(out + at, size - at, "%s%s", empty ? "" : ",", name);
            done = true;
        }
        out[at++] = '\n';
        line = end != NULL ? end + 1 : NULL;
    }
    out[at] = '\0';
    free(text);
    int err = 0;
    if (done)
        err = write_file("/etc/group", out, 0644, false);
    else {
        char line[128];
        snprintf(line, sizeof(line), "%s:x:%d:%s\n", group,
                 strcmp(group, "sudo") == 0 ? 27 : 100, name);
        err = append_line("/etc/group", line, 0644);
    }
    free(out);
    return err;
}

int native_root_add_user(const char *name, const char *password) {
    if (!native_root_user_name_valid(name))
        return db_has("/etc/passwd", name, -1) ? _EEXIST : _EINVAL;
    if (native_root_has_default_user())
        return _EEXIST;

    char hash[256] = "!";
    if (password != NULL && password[0] != '\0') {
        static const char alphabet[] =
            "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        unsigned char raw[16];
        if (get_random((char *) raw, sizeof(raw)) != 0)
            return _EIO;
        char salt[3 + sizeof(raw) + 1] = "$6$";
        for (size_t i = 0; i < sizeof(raw); i++)
            salt[3 + i] = alphabet[raw[i] % 64];
        salt[3 + sizeof(raw)] = '\0';
        if (aok_sha_crypt(password, salt, hash, sizeof(hash)) == NULL)
            return _EINVAL;
    }

    char line[512];
    int uid = NATIVE_ROOT_USER_UID;
    snprintf(line, sizeof(line), "%s:x:%d:%d:%s:/home/%s:%s\n",
             name, uid, uid, name, name, login_shell());
    int err = append_line("/etc/passwd", line, 0644);
    if (err < 0)
        return err;
    snprintf(line, sizeof(line), "%s:x:%d:\n", name, uid);
    if ((err = append_line("/etc/group", line, 0644)) < 0)
        return err;
    snprintf(line, sizeof(line), "%s:%s:%ld:0:99999:7:::\n", name, hash, days_since_epoch());
    if ((err = append_line("/etc/shadow", line, 0600)) < 0)
        return err;
    add_to_group("users", name);
    add_to_group("sudo", name);

    char home[MAX_PATH];
    snprintf(home, sizeof(home), "/home/%s", name);
    ensure_dir("/home", 0755);
    if (!path_exists(home))
        generic_mkdirat(AT_PWD, home, 0700);
    generic_setattrat(AT_PWD, home, make_attr(mode, 0700), false);
    generic_setattrat(AT_PWD, home, make_attr(uid, uid), false);
    generic_setattrat(AT_PWD, home, make_attr(gid, uid), false);
    return 0;
}
