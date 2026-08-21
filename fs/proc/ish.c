#include "fs/proc.h"
#include "fs/proc/ish.h"
#include "fs/proc/net.h"
#include "jit/jit.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#import <ifaddrs.h>
#import <netinet/in.h>
#import <sys/socket.h>
#import <unistd.h>
#if defined(__APPLE__)
#import <net/if_var.h>
#endif
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

const char *proc_ish_version = "";
char **(*get_all_defaults_keys)(void);
char *(*get_friendly_name)(const char *name);
char *(*get_underlying_name)(const char *name);
bool (*get_user_default)(const char *name, char **buffer, size_t *size);
bool (*set_user_default)(const char *name, char *buffer, size_t size);
bool (*remove_user_default)(const char *name);
char *(*get_documents_directory)(void);
char *(*ish_roots_status)(void);
int (*ish_roots_command)(const char *command);
char *(*ish_workspace_status)(void);
int (*ish_workspace_open)(const char *request);

#include "kernel/hostinfo.h"

static int proc_ish_show_colors(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf,
                "\x1B[30m" "iSH" "\x1B[39m "
                "\x1B[31m" "iSH" "\x1B[39m "
                "\x1B[32m" "iSH" "\x1B[39m "
                "\x1B[33m" "iSH" "\x1B[39m "
                "\x1B[34m" "iSH" "\x1B[39m "
                "\x1B[35m" "iSH" "\x1B[39m "
                "\x1B[36m" "iSH" "\x1B[39m "
                "\x1B[37m" "iSH" "\x1B[39m" "\n\x1B[7m"
                "\x1B[40m" "iSH" "\x1B[39m "
                "\x1B[41m" "iSH" "\x1B[39m "
                "\x1B[42m" "iSH" "\x1B[39m "
                "\x1B[43m" "iSH" "\x1B[39m "
                "\x1B[44m" "iSH" "\x1B[39m "
                "\x1B[45m" "iSH" "\x1B[39m "
                "\x1B[46m" "iSH" "\x1B[39m "
                "\x1B[47m" "iSH" "\x1B[39m" "\x1B[0m\x1B[1m\n"
                "\x1B[90m" "iSH" "\x1B[39m "
                "\x1B[91m" "iSH" "\x1B[39m "
                "\x1B[92m" "iSH" "\x1B[39m "
                "\x1B[93m" "iSH" "\x1B[39m "
                "\x1B[94m" "iSH" "\x1B[39m "
                "\x1B[95m" "iSH" "\x1B[39m "
                "\x1B[96m" "iSH" "\x1B[39m "
                "\x1B[97m" "iSH" "\x1B[39m" "\n\x1B[7m"
                "\x1B[100m" "iSH" "\x1B[39m "
                "\x1B[101m" "iSH" "\x1B[39m "
                "\x1B[102m" "iSH" "\x1B[39m "
                "\x1B[103m" "iSH" "\x1B[39m "
                "\x1B[104m" "iSH" "\x1B[39m "
                "\x1B[105m" "iSH" "\x1B[39m "
                "\x1B[106m" "iSH" "\x1B[39m "
                "\x1B[107m" "iSH" "\x1B[39m" "\x1B[0m\n"
                );
    return 0;
}

static int proc_ish_show_documents(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // get_documents_directory is installed by the iOS app; it is NULL in the
    // command-line build. Emit an empty value rather than calling through NULL
    // (stress-ng --procfs reads /proc/ish/documents and crashed the emulator).
    char *directory = get_documents_directory != NULL ? get_documents_directory() : NULL;
    proc_printf(buf, "%s\n", directory != NULL ? directory : "");
    free(directory);
    return 0;
}

// /proc/ish/roots -- the guest side of the app's Filesystems screen: which root
// filesystems are installed, which one is booted, which one boots next, and the
// commands to install another or change that choice. /AOK/tools/manage-roots.sh
// is the front end; this is the whole mechanism it drives.
//
//   cat /proc/ish/roots
//   printf 'op=default\nname=Devuan6-x86_64\nrun\n' > /proc/ish/roots
//
// The format is one key=value per LINE, and a value runs to the end of its
// line. Root names are the tame part (the app restricts those to
// [A-Za-z0-9._-]); the values that decide the format are the archive paths and
// download URLs, which contain spaces, '=' and '#' as a matter of course, and
// the job messages read back out, which are English sentences.
//
// A write is a FRAGMENT of a command, and the line `run` commits it. That is
// not ceremony: the shell's printf issues one write(2) per line, so the command
// above arrives as three separate writes, and treating each write as a whole
// command would reject every one of them. Fragments accumulate app-side.
//
// Nothing here blocks. Anything slow -- a download, an unpack -- becomes a job
// the app runs on its own queue, so the write returns at once and the caller
// watches `job` in the status. Only one job runs at a time; starting a second
// gets EBUSY.
static int proc_ish_show_roots(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // NULL in the command-line build, where there is no root manager at all.
    // Saying so beats calling through a NULL pointer: stress-ng --procfs reads
    // every file under here, and that is exactly how /proc/ish/documents once
    // took the whole emulator down.
    if (ish_roots_status == NULL) {
        proc_printf(buf, "job state=unavailable\n");
        proc_printf(buf, "job message=root management needs the iSH-AOK app\n");
        return 0;
    }
    char *status = ish_roots_status();
    if (status == NULL) {
        proc_printf(buf, "job state=error\n");
        proc_printf(buf, "job message=the root manager did not answer\n");
        return 0;
    }
    proc_printf(buf, "%s", status);
    free(status);
    return 0;
}

static int proc_ish_update_roots(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    if (ish_roots_command == NULL)
        return _EOPNOTSUPP;
    if (data->size == 0 || data->size > 8192)
        return _EINVAL;
    char *command = malloc(data->size + 1);
    if (command == NULL)
        return _ENOMEM;
    memcpy(command, data->data, data->size);
    command[data->size] = '\0';
    // An embedded NUL would end a value early on the app side while the parser
    // here saw the rest, so a command that should be rejected could arrive as a
    // different, accepted one. Refuse instead of guessing which half was meant.
    int err = strlen(command) == data->size ? ish_roots_command(command) : _EINVAL;
    free(command);
    return err;
}

// /proc/ish/workspace -- read it to learn whether this session is hosted by
// Workspace and which tools it can open, write to it to ask for one.
//
// Read format is key=value lines, the same shape as roots, so a shell can
// parse it with `. /dev/stdin`-free case matching:
//
//     hosted=1
//     tools=motepad,filemanager,markdown,imageviewer,videoplayer,audio,llm,...
//
// `hosted` answers "is there a Workspace that can receive a request", not "is
// THIS session inside one" -- and that is the useful question. An ssh login has
// no Workspace of its own but can still perfectly well ask the app on screen to
// open a file, which is exactly what someone typing ws-markdown over ssh wants.
//
// hosted=0 is a complete and useful answer, not an error: it is what a plain
// terminal session on a build with no Workspace, and the whole command-line
// build, honestly are. A launcher reads this first and falls back rather than
// writing a request nobody will answer.
//
// The entry is 0666 rather than roots' 0644: managing roots is administrative,
// opening a window is not, and every AOK session is uid 1000.
static int proc_ish_show_workspace(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // NULL in the command-line build and in any app build without Workspace.
    // Saying so beats calling through a NULL pointer -- stress-ng --procfs
    // reads every file under here, which is how /proc/ish/documents once took
    // the whole emulator down.
    if (ish_workspace_status == NULL) {
        proc_printf(buf, "hosted=0\n");
        proc_printf(buf, "reason=this build has no Workspace\n");
        return 0;
    }
    char *status = ish_workspace_status();
    if (status == NULL) {
        proc_printf(buf, "hosted=0\n");
        proc_printf(buf, "reason=Workspace did not answer\n");
        return 0;
    }
    proc_printf(buf, "%s", status);
    free(status);
    return 0;
}

// One verb today: `open <tool> [path]`. A verb set rather than a bare path is
// the point -- this is a guest asking the app to put something on screen, so
// what it may ask for has to be enumerable, and the app validates both the
// tool name and the path before acting on either.
static int proc_ish_update_workspace(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    if (ish_workspace_open == NULL)
        return _EOPNOTSUPP;
    if (data->size == 0 || data->size > 4096)
        return _EINVAL;
    char *request = malloc(data->size + 1);
    if (request == NULL)
        return _ENOMEM;
    memcpy(request, data->data, data->size);
    request[data->size] = '\0';
    // An embedded NUL would end the request early on the app side while the
    // parser here saw the rest, so a request that should be rejected could
    // arrive as a different, accepted one. Refuse rather than guess which half
    // was meant. (Same reasoning as proc_ish_update_roots.)
    int err = strlen(request) == data->size ? ish_workspace_open(request) : _EINVAL;
    free(request);
    return err;
}

static int proc_ish_show_amd64_jit(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s\n", amd64_jit_preference_get() ? "on" : "off");
    return 0;
}

static int proc_ish_show_i386_single_step_comm(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    char comm[16];
    i386_single_step_comm_get(comm, sizeof(comm));
    proc_printf(buf, "%s\n", comm);
    return 0;
}

static int proc_ish_show_i386_no_cache_comm(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    char comm[16];
    i386_no_cache_comm_get(comm, sizeof(comm));
    proc_printf(buf, "%s\n", comm);
    return 0;
}

// /proc/ish/{i386,arm64,riscv64,amd64}_jit_fuse -- read/write that guest's live
// gadget-fusion mask. One implementation, four nodes; the arch selects both the
// mask and the set of valid names, so a name from the wrong arch is rejected
// rather than silently ignored, and `all` means that arch's families only.
//
//   cat  /proc/ish/arm64_jit_fuse      -> one "name on|off" line per family
//   echo pushpop=0 > /proc/ish/i386_jit_fuse
//   echo "addr=0 alu=1" > /proc/ish/i386_jit_fuse    (space or comma separated)
//   echo all=1 > /proc/ish/riscv64_jit_fuse
//
// Families: i386 addr/movmr/lea/alu/pushpop; arm64 bcond/ldst/ldcmp;
// riscv64 fold/jal; amd64 incdec_reg (native gadget vs. the C-helper bridge,
// see jit/jit.h -- same A/B, different mechanism).
//
// Exists for measurement: it makes a fusion A/B a file write instead of an app
// relaunch, so arms can be interleaved rep by rep (the only way to keep thermal
// drift out of a 1-2% result), and the value written is the same variable gen.c
// reads, so cat-ing it back is proof the change took effect. Affects newly
// compiled blocks; run each rep as its own guest process to pick up a change.
// See jit/jit.h.
static int proc_ish_show_jit_fuse(enum jit_fuse_arch arch, struct proc_data *buf) {
    unsigned mask = jit_fuse_mask_get(arch);
    for (unsigned i = 0;; i++) {
        unsigned bit;
        const char *name = jit_fuse_name(arch, i, &bit);
        if (name == NULL)
            break;
        proc_printf(buf, "%s %s\n", name, (mask & bit) ? "on" : "off");
    }
    return 0;
}

static int proc_ish_update_jit_fuse(enum jit_fuse_arch arch, struct proc_data *data) {
    // Deliberately requires an explicit =0/=1 per name rather than treating a
    // bare name as "disable": the ISH_NO_*_FUSE env vars use presence-means-off,
    // and mistaking `FOO=0` for "off" there has already cost one bogus A/B.
    // Nothing is applied unless the WHOLE input parses, so a typo cannot leave
    // the mask half-changed.
    unsigned mask = jit_fuse_mask_get(arch);
    size_t i = 0;
    bool saw_one = false;
    while (i < data->size) {
        char c = data->data[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
            i++;
            continue;
        }
        size_t start = i;
        while (i < data->size) {
            c = data->data[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',')
                break;
            i++;
        }
        size_t len = i - start;
        // "name=v": at least one name char, an '=', and a single 0 or 1.
        if (len < 3 || data->data[i - 2] != '=')
            return _EINVAL;
        char val = data->data[i - 1];
        if (val != '0' && val != '1')
            return _EINVAL;
        size_t namelen = len - 2;
        if (namelen >= 32)
            return _EINVAL;
        char name[32];
        memcpy(name, &data->data[start], namelen);
        name[namelen] = '\0';

        unsigned bit = 0;
        bool matched = false;
        if (strcmp(name, "all") == 0) {
            // Route "all" through the shared setter so each arch gets ITS OWN
            // full-on value rather than i386's bit set.
            jit_fuse_set_by_name(arch, "all", val == '1');
            mask = jit_fuse_mask_get(arch);
            matched = true;
        } else {
            for (unsigned k = 0;; k++) {
                const char *known = jit_fuse_name(arch, k, &bit);
                if (known == NULL)
                    break;
                if (strcmp(name, known) == 0) {
                    if (val == '1')
                        mask |= bit;
                    else
                        mask &= ~bit;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched)
            return _EINVAL;
        saw_one = true;
    }
    if (!saw_one)
        return _EINVAL;
    jit_fuse_mask_set(arch, mask);
    return 0;
}

// One node per guest, named for the arch it controls so nobody expects
// i386_jit_fuse to touch arm64 (matching the i386_no_cache_comm convention).
// proc_entry carries no user data, so each node needs a two-line wrapper.
static int proc_ish_show_i386_jit_fuse(struct proc_entry *UNUSED(e), struct proc_data *buf) {
    return proc_ish_show_jit_fuse(JIT_FUSE_ARCH_I386, buf);
}
static int proc_ish_update_i386_jit_fuse(struct proc_entry *UNUSED(e), struct proc_data *d) {
    return proc_ish_update_jit_fuse(JIT_FUSE_ARCH_I386, d);
}
static int proc_ish_show_arm64_jit_fuse(struct proc_entry *UNUSED(e), struct proc_data *buf) {
    return proc_ish_show_jit_fuse(JIT_FUSE_ARCH_ARM64, buf);
}
static int proc_ish_update_arm64_jit_fuse(struct proc_entry *UNUSED(e), struct proc_data *d) {
    return proc_ish_update_jit_fuse(JIT_FUSE_ARCH_ARM64, d);
}
static int proc_ish_show_riscv64_jit_fuse(struct proc_entry *UNUSED(e), struct proc_data *buf) {
    return proc_ish_show_jit_fuse(JIT_FUSE_ARCH_RISCV64, buf);
}
static int proc_ish_update_riscv64_jit_fuse(struct proc_entry *UNUSED(e), struct proc_data *d) {
    return proc_ish_update_jit_fuse(JIT_FUSE_ARCH_RISCV64, d);
}
static int proc_ish_show_amd64_jit_fuse(struct proc_entry *UNUSED(e), struct proc_data *buf) {
    return proc_ish_show_jit_fuse(JIT_FUSE_ARCH_AMD64, buf);
}
static int proc_ish_update_amd64_jit_fuse(struct proc_entry *UNUSED(e), struct proc_data *d) {
    return proc_ish_update_jit_fuse(JIT_FUSE_ARCH_AMD64, d);
}

static int proc_ish_update_amd64_jit(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    size_t start = 0;
    size_t end = data->size;

    while (start < end && (data->data[start] == ' ' || data->data[start] == '\t' ||
            data->data[start] == '\r' || data->data[start] == '\n'))
        start++;
    while (end > start && (data->data[end - 1] == ' ' || data->data[end - 1] == '\t' ||
            data->data[end - 1] == '\r' || data->data[end - 1] == '\n'))
        end--;

    if (end - start != 1)
        return _EINVAL;
    if (data->data[start] == '0') {
        amd64_jit_preference_set(false);
        return 0;
    }
    if (data->data[start] == '1') {
        amd64_jit_preference_set(true);
        return 0;
    }
    return _EINVAL;
}

static int proc_ish_update_i386_single_step_comm(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    size_t start = 0;
    size_t end = data->size;

    while (start < end && (data->data[start] == ' ' || data->data[start] == '\t' ||
            data->data[start] == '\r' || data->data[start] == '\n'))
        start++;
    while (end > start && (data->data[end - 1] == ' ' || data->data[end - 1] == '\t' ||
            data->data[end - 1] == '\r' || data->data[end - 1] == '\n'))
        end--;

    size_t len = end - start;
    if (len >= 16)
        return _EINVAL;

    char comm[16];
    memcpy(comm, &data->data[start], len);
    comm[len] = '\0';
    i386_single_step_comm_set(comm);
    return 0;
}

static int proc_ish_update_i386_no_cache_comm(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    size_t start = 0;
    size_t end = data->size;

    while (start < end && (data->data[start] == ' ' || data->data[start] == '\t' ||
            data->data[start] == '\r' || data->data[start] == '\n'))
        start++;
    while (end > start && (data->data[end - 1] == ' ' || data->data[end - 1] == '\t' ||
            data->data[end - 1] == '\r' || data->data[end - 1] == '\n'))
        end--;

    size_t len = end - start;
    if (len >= 16)
        return _EINVAL;

    char comm[16];
    memcpy(comm, &data->data[start], len);
    comm[len] = '\0';
    i386_no_cache_comm_set(comm);
    return 0;
}

static void proc_ish_defaults_getname(struct proc_entry *entry, char *buf) {
    strcpy(buf, entry->name);
}

static int proc_ish_defaults_readlink(struct proc_entry *entry, char *buf) {
    if (get_underlying_name == NULL)
        return _EIO;
    char *name = get_underlying_name(entry->name);
    snprintf(buf, MAX_PATH, "../.defaults/%s", name);
    free(name);
    return 0;
}

static int proc_ish_underlying_defaults_show(struct proc_entry *entry, struct proc_data *data) {
    size_t size;
    char *buffer;
    if (get_user_default == NULL)
        return _EIO;
    if (!get_user_default(entry->name, &buffer, &size))
        return _EIO;
    proc_buf_append(data, buffer, size);
    free(buffer);
    return 0;
}

static int proc_ish_underlying_defaults_update(struct proc_entry *entry, struct proc_data *data) {
    if (set_user_default == NULL)
        return _EIO;
    if (!set_user_default(entry->name, data->data, data->size))
        return _EIO;
    return 0;
}

static int proc_ish_underlying_defaults_unlink(struct proc_entry *entry) {
    if (remove_user_default == NULL)
        return _EIO;
    return remove_user_default(entry->name) ? 0 : _EIO;
}

static int proc_ish_defaults_unlink(struct proc_entry *entry) {
    if (get_underlying_name == NULL || remove_user_default == NULL)
        return _EIO;
    char *name = get_underlying_name(entry->name);
    int err = remove_user_default(name) ? 0 : _EIO;
    free(name);
    return err;
}

struct proc_dir_entry proc_ish_underlying_defaults_fd = { NULL,
    .getname = proc_ish_defaults_getname,
    .show = proc_ish_underlying_defaults_show,
    .update = proc_ish_underlying_defaults_update,
    .unlink = proc_ish_underlying_defaults_unlink,
};

struct proc_dir_entry proc_ish_defaults_fd = { NULL, S_IFLNK,
    .getname = proc_ish_defaults_getname,
    .readlink = proc_ish_defaults_readlink,
    .unlink = proc_ish_defaults_unlink,
};

static void get_child_names(struct proc_entry *entry, unsigned long index) {
    if (index == 0 || entry->child_names == NULL) {
        if (entry->child_names != NULL)
            free_string_array(entry->child_names);
        // get_all_defaults_keys is installed by the iOS app (UserPreferences.m);
        // it is NULL in the command-line build. Fall back to an empty (but
        // valid, NULL-terminated) listing rather than calling through NULL --
        // reading /proc/ish/defaults otherwise crashed the emulator.
        entry->child_names = get_all_defaults_keys != NULL ? get_all_defaults_keys() : NULL;
        if (entry->child_names == NULL) {
            entry->child_names = malloc(sizeof(char *));
            if (entry->child_names != NULL)
                entry->child_names[0] = NULL;
        }
    }
}

static bool proc_ish_underlying_defaults_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    get_child_names(entry, *index);
    if (entry->child_names == NULL || entry->child_names[*index] == NULL)
        return false;
    next_entry->meta = &proc_ish_underlying_defaults_fd;
    next_entry->name = strdup(entry->child_names[*index]);
    (*index)++;
    return true;
}

static bool proc_ish_defaults_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    get_child_names(entry, *index);
    if (get_friendly_name == NULL || entry->child_names == NULL)
        return false;
    char *friendly_name;
    do {
        const char *name = entry->child_names[*index];
        if (name == NULL)
            return false;
        friendly_name = get_friendly_name(name);
        (*index)++;
    } while (friendly_name == NULL);
    next_entry->meta = &proc_ish_defaults_fd;
    next_entry->name = friendly_name;
    return true;
}

char *get_ip_str(const struct sockaddr *sa, char *s, socklen_t maxlen) {
    switch(sa->sa_family) {
        case AF_INET:
            inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr), s, maxlen);
            break;

        case AF_INET6:
            inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr), s, maxlen);
            break;

        default:
            strncpy(s, "Unknown AF", maxlen);
            return NULL;
    }

    return s;
}

#include <string.h>
#include <stdlib.h>
#include <net/if.h>  // for the IFF_* flags

#define FLAG_MAP_ENTRY(f, s) { f, s, sizeof(s) - 1 }

char *parse_if_flags(int flags) {
    int first = 1;
    char *build_string = malloc(200);
    
    if (build_string == NULL) {
        return NULL; // Allocation failed
    }
    
    struct {
        int flag;
        const char *str;
        size_t len;
    } flag_str_map[] = {
        FLAG_MAP_ENTRY(IFF_UP, "UP"),
        FLAG_MAP_ENTRY(IFF_BROADCAST, "BROADCAST"),
        FLAG_MAP_ENTRY(IFF_DEBUG, "DEBUG"),
        FLAG_MAP_ENTRY(IFF_LOOPBACK, "LOOPBACK"),
        FLAG_MAP_ENTRY(IFF_POINTOPOINT, "POINTOPOINT"),
        FLAG_MAP_ENTRY(IFF_NOTRAILERS, "NOTRAILERS"),
        FLAG_MAP_ENTRY(IFF_RUNNING, "RUNNING"),
        FLAG_MAP_ENTRY(IFF_NOARP, "NOARP"),
        FLAG_MAP_ENTRY(IFF_PROMISC, "PROMISC"),
        FLAG_MAP_ENTRY(IFF_ALLMULTI, "ALLMULTI"),
        FLAG_MAP_ENTRY(IFF_MULTICAST, "MULTICAST"),
    };

    size_t len = 0;
    for (size_t i = 0; i < sizeof(flag_str_map)/sizeof(flag_str_map[0]); ++i) {
        if (flags & flag_str_map[i].flag) {
            if (!first) {
                build_string[len++] = ',';
            }
            memcpy(build_string + len, flag_str_map[i].str, flag_str_map[i].len);
            len += flag_str_map[i].len;
            first = 0;
        }
    }
    build_string[len] = '\0';

    return build_string;
}

static int proc_ish_show_ips(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "Iface        IP                                         Broadcast/Multicast    Family    Flags\n");

    struct ifaddrs *addrs;
    int ret = getifaddrs(&addrs);
    if (ret != 0) {
        return -1; // Or another form of error reporting
    }

    struct ifaddrs *cursor = addrs;
    char type[9];
    
    while (cursor != NULL) {
        if ((cursor->ifa_addr->sa_family == AF_INET) || (cursor->ifa_addr->sa_family == AF_INET6)) {
            char int_ip[100];
            char int_dstaddr[100];

            if (cursor->ifa_addr->sa_family == AF_INET) {
                strncpy(type, "IF_INET", sizeof(type));
            } else {
                strncpy(type, "IF_INET6", sizeof(type));
            }
            type[sizeof(type) - 1] = '\0';
            
            get_ip_str(cursor->ifa_addr, int_ip, sizeof(int_ip));
            
            if (cursor->ifa_dstaddr != NULL) {
                get_ip_str(cursor->ifa_dstaddr, int_dstaddr, sizeof(int_dstaddr));
            } else {
                strcpy(int_dstaddr, " ");
            }

            char int_flags[250];
            char *parsed_flags = parse_if_flags(cursor->ifa_flags);
            if (parsed_flags) {
                strncpy(int_flags, parsed_flags, sizeof(int_flags));
                int_flags[sizeof(int_flags) - 1] = '\0';
                free(parsed_flags);
            } else {
                int_flags[0] = '\0';
            }
            
            proc_printf(buf, "%-10.10s   %-40s   %-40s   %-8s  %-60s\n",
                        cursor->ifa_name,
                        int_ip,
                        int_dstaddr,
                        type,
                        int_flags
            );
        }
        cursor = cursor->ifa_next;
    }

    freeifaddrs(addrs);
    return 0;
}

static int proc_ish_show_version(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s\n", proc_ish_version);
    return 0;
}

extern char* printBatteryStatus(int type);

static int proc_ish_show_battery(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s", printBatteryStatus(3));
    return 0;
}

static int proc_ish_show_battery_capacity(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s", printBatteryStatus(2));
    return 0;
}

static int proc_ish_show_battery_status(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s", printBatteryStatus(1));
    return 0;
}

extern char* printUIDevice(void);

static int proc_ish_show_uidevice(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s", printUIDevice());
    return 0;
}

static int proc_ish_show_host_info(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    char *host_info = printHostInfo();
    proc_printf(buf, "%s", host_info);
    free(host_info);
    return 0;
}

struct proc_children proc_ish_children = PROC_CHILDREN({
    {"amd64_jit", S_IFREG | 0644, .show = proc_ish_show_amd64_jit, .update = proc_ish_update_amd64_jit},
    {"amd_jit", S_IFREG | 0644, .show = proc_ish_show_amd64_jit, .update = proc_ish_update_amd64_jit},
    {"amd64_jit_fuse", S_IFREG | 0644, .show = proc_ish_show_amd64_jit_fuse, .update = proc_ish_update_amd64_jit_fuse},
    {"arm64_jit_fuse", S_IFREG | 0644, .show = proc_ish_show_arm64_jit_fuse, .update = proc_ish_update_arm64_jit_fuse},
    {"i386_jit_fuse", S_IFREG | 0644, .show = proc_ish_show_i386_jit_fuse, .update = proc_ish_update_i386_jit_fuse},
    {"riscv64_jit_fuse", S_IFREG | 0644, .show = proc_ish_show_riscv64_jit_fuse, .update = proc_ish_update_riscv64_jit_fuse},
    {"i386_no_cache_comm", S_IFREG | 0644, .show = proc_ish_show_i386_no_cache_comm, .update = proc_ish_update_i386_no_cache_comm},
    {"i386_single_step_comm", S_IFREG | 0644, .show = proc_ish_show_i386_single_step_comm, .update = proc_ish_update_i386_single_step_comm},
    {"BAT0", .show = proc_ish_show_battery},
    {"BAT0_capacity", .show = proc_ish_show_battery_capacity},
    {"BAT0_status", .show = proc_ish_show_battery_status},
    {"UIDevice", .show = proc_ish_show_uidevice},
    {"colors", .show = proc_ish_show_colors},
    {".defaults", S_IFDIR, .readdir = proc_ish_underlying_defaults_readdir},
    {"defaults", S_IFDIR, .readdir = proc_ish_defaults_readdir},
    {"documents", .show = proc_ish_show_documents},
    {"host_info", .show = proc_ish_show_host_info},  // Add host hardware related information
    {"ips", .show = proc_ish_show_ips},
    {"roots", S_IFREG | 0644, .show = proc_ish_show_roots, .update = proc_ish_update_roots},
    {"workspace", S_IFREG | 0666, .show = proc_ish_show_workspace, .update = proc_ish_update_workspace},
    {"version", .show = proc_ish_show_version},
});

void proc_ish_init(struct proc_dir_entry *root_entry) {
    struct proc_dir_entry *defaults_dir;
    struct proc_dir_entry *underlying_defaults_dir;

    if (root_entry == NULL)
        return;

    proc_set_children_parent(&proc_ish_children, root_entry);

    underlying_defaults_dir = proc_children_find(&proc_ish_children, ".defaults");
    if (underlying_defaults_dir != NULL)
        proc_ish_underlying_defaults_fd.parent = underlying_defaults_dir;

    defaults_dir = proc_children_find(&proc_ish_children, "defaults");
    if (defaults_dir != NULL)
        proc_ish_defaults_fd.parent = defaults_dir;
}
