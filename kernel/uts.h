#ifndef KERNEL_UTS_H
#define KERNEL_UTS_H

#include <stdbool.h>
#include <stddef.h>
#include "util/sync.h"

// A UTS namespace holds exactly two strings: the nodename (hostname) and the
// NIS domainname. That's the whole of it in real Linux too, which is why this
// is the one namespace iSH can implement honestly -- systemd creates a UTS
// namespace for every transient unit (systemd-run), and refusing
// unshare(CLONE_NEWUTS) made it fail with 226/NAMESPACE before it ever got
// as far as the namespaces we really can't do (issue #527).
//
// An empty hostname means "not set in this namespace"; readers then fall back
// to the host's nodename, preserving the behavior iSH had before hostnames
// were namespaced.
#define UTS_NAME_LENGTH 65 // must match UNAME_LENGTH in kernel/calls.h

struct uts_namespace {
    lock_t lock;
    unsigned refcount; // guarded by lock
    char hostname[UTS_NAME_LENGTH];
    char domainname[UTS_NAME_LENGTH];
};

// The namespace every task starts in, and the one the app-side hostname
// plumbing writes to. Statically allocated so it is usable before any task
// exists (and so its refcount never reaching zero is harmless).
extern struct uts_namespace init_uts_ns;

struct uts_namespace *uts_ns_retain(struct uts_namespace *ns);
void uts_ns_release(struct uts_namespace *ns);
// Snapshot `ns` into a fresh namespace with refcount 1, for CLONE_NEWUTS.
struct uts_namespace *uts_ns_copy(struct uts_namespace *ns);

// The namespace of the calling task, or the initial one when called off a
// task thread (procfs helpers, app startup).
struct uts_namespace *uts_ns_current(void);

// Set the initial namespace's hostname from the host side (app settings,
// /etc/hostname seeding). Truncates as sethostname
// would; a NULL or empty name clears the override.
void uts_set_boot_hostname(const char *hostname);
bool uts_boot_hostname_is_set(void);

#endif
