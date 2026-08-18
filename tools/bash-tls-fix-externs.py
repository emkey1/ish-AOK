#!/usr/bin/env python3
"""Make bash's remaining shared globals thread-local, by name, everywhere.

The companion tools each miss this set for their own reason.
bash-tls-rewrite.py drives off clang's JSON AST, whose file attribution drifts
(see the comment there) so it silently skipped a batch. bash-tls-fix-statics.py
only looks at non-external symbols. check-bash-tls.py only reports symbols two
files DISAGREE about, and a variable no file made thread-local is something
every file agrees on -- consistently shared, consistently wrong.

What was left behind mattered. o_options is one of the tables aok_fix_*() fills
in per thread: shared, every shell writes its own variables' addresses into the
one table and the last writer wins, which is precisely the race this conversion
exists to remove. xpg_echo, localvar_unset and shell_function_defs are ordinary
per-shell settings that one shell would have changed under another.

So this takes the names from `nm` -- external, mutable, not thread-local -- and
inserts __thread at every file-scope declaration of them in the vendored tree,
definitions and externs alike. A declaration missed in one header is not a
compile error, it is a silent wrong-memory read, which is why it goes by name
across every file rather than per translation unit.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.path.join(REPO, "deps", "bash")

# Deliberately shared. Each of these is something two live shells can see each
# other through, kept that way for a reason that is worth more than the
# isolation would be.
ALLOW = {
    # readline's keymaps cross-reference each other in static initialisers
    # (emacs_standard_keymap's Control-x entry IS emacs_ctlx_keymap), so they
    # cannot be thread-local without a fixup pass over all of them. The visible
    # consequence is narrow and interactive: `bind` in one shell is seen by
    # another. Noted in docs/bash_native_plan.md.
    "emacs_standard_keymap", "emacs_meta_keymap", "emacs_ctlx_keymap",
    "vi_movement_keymap", "vi_insertion_keymap", "builtin_keymap_names",
    # The set of builtins bash was compiled with -- identical in every shell.
    # `enable`/`disable` flip flags inside it, which is shared; see builtins.h.
    "static_shell_builtins", "shell_builtins", "num_shell_builtins",
    "current_builtin",
    # Written once, at startup, with the same value by every shell.
    "rl_library_version", "rl_readline_name",
    # Read-only lookup tables that only lack `const` upstream.
    "default_prefixes", "default_suffixes",
    "posix_collsyms", "posix_collwcsyms",
    # A clipboard is shared BY DEFINITION -- `pbcopy` in one shell then
    # `pbpaste` in another. Per-task would give every applet its own empty one.
    # Guarded by g_clipboard_lock in runtime_support.c, which concurrent applets
    # made necessary rather than merely tidy.
    "g_clipboard_data", "g_clipboard_init", "g_clipboard_len",
    # The mutex guarding them. A per-thread lock guards nothing.
    "g_clipboard_lock",

    # ---- smallclue (build/libsmallclue.a) ----
    # Its applets are native programs for the same reason bash is, so two live
    # ones share every global. The surface is far smaller: 36 mutable symbols
    # against bash's ~1500, and 24 already thread-local.
    #
    # Read-only tables that only lack `const`, same class as the bash ones above.
    "kAwkBuiltins", "kSmallclueAppletCount",
    "markdownLooksLikeNewsMetaLine.months", "smallclueBaudLabel.unknown",
    "smallclueDfFormatSize.suffixes", "smallclueDnsRcodeName.rcodeNames",
    "smallclueTouchParseDashD.formats",
    # nextvi's session registry is shared ON PURPOSE and carries its own lock
    # (s_nextvi_sessions_lock). Making it per-thread would give every task its
    # own empty registry and quietly defeat session sharing, which is the point
    # of it.
    "s_nextvi_sessions", "s_nextvi_session_count", "s_nextvi_session_cap",
    "s_nextvi_sessions_lock",
    # The app's start time, captured by a __attribute__((constructor)) that runs
    # once on the loading thread. Thread-local gave every applet a zero and
    # `uptime` printed "up 00:00:00" against the distro's "up 4:32" -- the name
    # was right and the conversion was not. Written before any applet runs and
    # read-only after, so sharing it needs no lock.
    "gSmallclueProcessStartMonoNs",

    # ---- zsh (build/libzsh.a) ----
    # zsh is a native program for the same reason bash is, and needed the same
    # conversion before fork-by-re-launch could target /AOK/native/zsh: the
    # child of a re-launch is a second live zsh in the same address space.
    #
    # widgets[] and thingies[] USED to be excused here, on the grounds that
    # they cross-reference each other's ADDRESSES in static initialisers, and
    # that the damage was confined to interactive shells. Both halves were
    # true and the conclusion was wrong: interactive shells are what a shell is
    # for, and two AOK terminal tabs running zsh shared one mutable widget
    # table, so `zle -N accept-line ...` in one landed in the other's key table
    # and the second shell's init_thingies() re-chained the nodes out from
    # under the first's hash table. They are now deferred-init thread-locals
    # (Src/Zle/zle_bindings.c, and the long comment on thingies[] in
    # Src/Zle/zle.h), which is what this file's own zsh-tls-fix-tables.py
    # companion exists to do. Do not add them back.
    #
    # A table of zsh's own execution functions, indexed by wordcode type. Read
    # on every command; assigned never. Only lacks `const` upstream.
    "execfuncs",
    # ksh93.c: `static char sh_unsetval[2]` -- a never-written sentinel whose
    # ADDRESS is the value ("Dummy to treat as NULL"). Three thread-locals are
    # initialised with it, and the address of a thread-local is not a constant
    # expression, so making this one per-thread breaks the file for no gain.
    "sh_unsetval",
    # termquery.c: the all-NULL array returned when prompt markers are off.
    "prompt_markers.nomark",

    # ---- OpenSSH (build/libopenssh*.a) ----
    # ssh, scp, sftp and ssh-keygen are native programs too, so one run's
    # file-statics were still there for the next one -- the reported bug was
    # ssh-keygen's `identity_file` surviving from a root run into a user run.
    #
    # Read-only lookup tables that only lack `const` upstream. Nothing writes
    # to any of these; each is a name/value table walked to translate a string.
    "keywords",                    # readconf.c: config keyword -> OpCode
    "log_facilities", "log_levels",  # log.c: name -> SyslogFacility/LogLevel
    "cclasses",                    # openbsd-compat/charclass.h: [:alpha:] & co
    "PADDING",                     # openbsd-compat/md5.c: the MD5 pad block
    # sntrup761's constant-time primitives XOR/AND with these to stop the
    # compiler proving a branch away; they are read on every operation and
    # assigned never. Thread-local would put a TLS lookup in the inner loop of
    # the key exchange to isolate a value that is always zero.
    "crypto_int16_optblocker", "crypto_int32_optblocker",
    "crypto_int64_optblocker",
    # An upstream slip, not state: umac.c writes `} umac_ctx;` after the struct
    # definition, defining an unused global instance of it. (umac128.c is the
    # same file compiled with the names remapped, hence the second one.) No
    # code reads or writes either.
    "umac_ctx", "umac128_ctx",
}

# Undecided: the fixers leave these alone, and the gate KEEPS REPORTING them.
#
# This is a third state on purpose. ALLOW means "shared, and that is right";
# putting an open question in there would record a decision nobody made, and the
# gate would go quiet on it. Leaving it out of ALLOW entirely is worse the other
# way -- the fixers would convert it, which is also a decision nobody made. So:
# skipped by the fixers, still reported by the gate, until someone answers the
# question written next to it.
    # Empty, and that is the point: an entry here is a question nobody has
    # answered yet, not a parking space. Both original entries were closed
    # rather than left to rot.
    #
    #   g_pager_sigwinch_received -- asked which thread a native program's
    #     signal arrives on. Answer: its own. The shim records the handler and
    #     calls it from native_checkpoint -> nlibc_deliver_signals on the thread
    #     making the syscall, so __thread is correct rather than merely
    #     harmless. Converted.
    #   g_clipboard_* -- asked share-with-a-lock versus per-applet. Answer:
    #     shared, obviously, since `pbcopy` in one shell and `pbpaste` in
    #     another is what a clipboard IS; per-task would give each applet its
    #     own empty one. Now guarded by a mutex, because Set() frees and
    #     reallocates while a concurrent Get() may be copying -- a
    #     use-after-free, not a stale read. Moved to ALLOW.
DEFER = set()

# What the fixers skip. The gate skips only ALLOW.
SKIP = ALLOW | DEFER

# .epro and .pro are zsh's: makepro.awk generates one per source file and that
# is where zsh keeps the `extern` for every global it exports -- Src/params.epro
# holds `extern char **pparams;` and Src/zsh.mdh includes the lot. A header the
# tool does not walk is a declaration left non-thread-local, which is the silent
# wrong-memory read this whole conversion exists to avoid, so they are not
# optional. They are generated files that are checked in (see the note at the
# top of meson.build about deps/zsh being a CONFIGURED tree); re-running zsh's
# `make prep` would undo this, and re-running this tool is the fix.
SRC = (".c", ".h", ".def", ".y", ".epro", ".pro")


def shared_externals(archive):
    names = set()
    with tempfile.TemporaryDirectory() as tmp:
        # Absolute: `ar x` runs with cwd=tmp, so a relative archive path
        # resolves against the temp directory, finds nothing, and -- because the
        # output is captured -- reports "nothing shared" instead of failing.
        subprocess.run(["ar", "x", os.path.abspath(archive)], cwd=tmp,
                       capture_output=True)
        for entry in sorted(os.listdir(tmp)):
            if not entry.endswith(".o"):
                continue
            nm = subprocess.run(["nm", "-m", os.path.join(tmp, entry)],
                                capture_output=True, text=True).stdout
            for line in nm.splitlines():
                # `external` is not adjacent to the section: a common symbol
                # prints as `(common) (alignment 2^3) external _name`. Matching
                # them as one string quietly skipped every tentative definition
                # -- tempvar_list, pcomp_curcmd, rl_executing_keymap and the
                # rest -- and reported "nothing shared" while seven were.
                if not re.search(r"\((__DATA,__(data|bss|common)|common)\)", line):
                    continue
                if " external" not in line or "non-external" in line:
                    continue
                sym = line.split()[-1]
                if "$tlv$" in sym or not sym.startswith("_"):
                    continue
                name = sym[1:]
                if name not in SKIP:
                    names.add(name)
    return names


# Archives whose sources are NOT at deps/<name>.
#
# OpenSSH is a submodule inside a submodule -- deps/smallclue/third-party/openssh
# -- so the deps/<name> guess lands on nothing and falls through to deps/bash,
# where every OpenSSH symbol is reported MISSED. meson also splits it across
# several archives, because scp.c and smult_curve25519_ref.c each need a
# compiler flag no other file may see (see openssh_special in meson.build), and
# all of those share the one tree. libopenssh_stubs.a is the exception: it is
# SmallCLUE's own glue file, deps/smallclue/src/openssh_stubs.c.
OPENSSH = os.path.join(REPO, "deps", "smallclue", "third-party", "openssh")
VENDOR_OVERRIDE = {
    "openssh_stubs": os.path.join(REPO, "deps", "smallclue"),
}


def vendor_for(archive):
    """Which vendored tree an archive's sources live in.

    libbash.a -> deps/bash, libsmallclue.a -> deps/smallclue. Walking the wrong
    one finds no declarations and reports every symbol as MISSED, which looks
    like a parsing bug rather than the wrong root.
    """
    stem = os.path.basename(archive)
    if stem.startswith("lib") and stem.endswith(".a"):
        name = stem[3:-2]
        if name in VENDOR_OVERRIDE:
            return VENDOR_OVERRIDE[name]
        if name == "openssh" or name.startswith("openssh_"):
            return OPENSSH
        cand = os.path.join(REPO, "deps", name)
        if os.path.isdir(cand):
            return cand
    return VENDOR


# Directories inside a vendored tree that are not compiled into the app.
# OpenSSH's contrib/ and regress/ are excluded by the same names in
# meson.build; patching them would put __thread on declarations no build ever
# sees, which is pure churn in a submodule fork whose diff has to stay readable.
UNBUILT = (os.sep + "contrib" + os.sep, os.sep + "regress" + os.sep)


def sources(root=None):
    for root, _, files in os.walk(root or VENDOR):
        if os.sep + ".git" in root:
            continue
        if any(u in root + os.sep for u in UNBUILT):
            continue
        for f in files:
            if f.endswith(SRC):
                yield os.path.join(root, f)


def declares(line, name):
    """True if this file-scope line declares `name`.

    Narrow on purpose. The name must be a declarator: it is followed by
    optional array brackets and then `;`, `=` or `,`, and nothing opens a
    paren before it -- `int f (int xpg_echo)` names a parameter and
    `static int g PARAMS((int))` is a prototype, and putting __thread on either
    does not compile.
    """
    # A line opening a block comment is not a declaration, and OpenSSH writes
    # plenty of them at column 0. `/* ... for a given host, but skip ... */`
    # matched the declarator pattern on "host," and the file got
    # `__thread /* print all known host keys ...` above a function.
    if line.startswith(("#", "//", "/*", " ", "\t", "}", "*")) or "__thread" in line:
        return False
    m = re.search(r"\b" + re.escape(name) + r"\b\s*(\[[^\]]*\])*\s*[;=,]", line)
    if not m:
        return False
    head = line[:m.start()]
    if head.count("(") > head.count(")"):
        return False
    # A typedef or a struct member is not a variable.
    return not re.match(r"\s*(typedef|return|case)\b", line)


def main():
    archive = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "libbash.a")
    names = shared_externals(archive)
    if not names:
        print("bash-tls-fix-externs: nothing shared", file=sys.stderr)
        return 0
    print(f"bash-tls-fix-externs: {len(names)} shared externals: "
          f"{' '.join(sorted(names))}", file=sys.stderr)

    hits, changed = {n: 0 for n in names}, 0
    for path in sources(vendor_for(archive)):
        with open(path, encoding="utf-8", errors="surrogateescape") as fh:
            lines = fh.read().split("\n")
        touched = False
        for i, line in enumerate(lines):
            for n in names:
                if n in line and declares(line, n):
                    lines[i] = insert_thread(line)
                    hits[n] += 1
                    touched = True
                    break
        if touched:
            changed += 1
            with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
                fh.write("\n".join(lines))

    for n in sorted(names):
        if hits[n] == 0:
            print(f"  MISSED {n}: no declaration found", file=sys.stderr)
    print(f"  patched {sum(hits.values())} declarations in {changed} files",
          file=sys.stderr)
    return 1 if any(v == 0 for v in hits.values()) else 0


if __name__ == "__main__":
    sys.exit(main())


def insert_thread(line):
    """Put `__thread` AFTER a leading extern/static, never before it.

    GCC requires __thread to follow the storage class and rejects the other
    order outright -- "error: '__thread' before 'extern'" -- while clang accepts
    both. Since the app is built with clang, `__thread extern` compiled here for
    months and only ever failed on the Linux CI builds, which are gcc and clang
    on a platform nobody was watching because the build died earlier for
    unrelated reasons. 527 declarations were affected.
    """
    if "__thread" in line:
        return line
    stripped = line.lstrip()
    indent = line[:len(line) - len(stripped)]
    for kw in ("extern ", "static "):
        if stripped.startswith(kw):
            return indent + kw + "__thread " + stripped[len(kw):]
    return indent + "__thread " + stripped
