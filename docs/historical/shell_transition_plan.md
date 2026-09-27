# Native shells: bash out, dash in, zsh as the one that checkpoints (archived, not maintained)

**This file is history.** This records a maintainer decision from 2026-09-10
to remove native bash outright (decisions #2/#5: "removed in 556",
`/AOK/native/bash` becomes a symlink). That removal did not land, or was
reverted: `kernel/native.c` still registers `bash` as a real native program,
and `meson_options.txt`'s `native_bash` option is a full, documented
disabled-by-default feature, not a removed one. The two documents that
currently govern this — [`README.md`](../../README.md#native-bash-and-licensing)
and [`docs/llm_onboarding.md`](../llm_onboarding.md) — both correctly describe
native bash as merely **disabled by default**, still in the tree. Kept because
this file's dash `mksignames.c` GPL-2+ research is real and is the basis for
`meson.build`'s current build guard, even though the bash-removal decision it
sits alongside did not stick.

---

# Native shells: bash out, dash in, zsh as the one that checkpoints

Maintainer's call, 2026-09-10. Four decisions and one finding that qualifies
the fourth.

## Why this exists

The App Store cannot take GPL. That is not a preference or a licence-purity
argument: it is the VLC precedent, where GPLv2 software was pulled from the
store because the store's usage terms conflict with the licence's. **bash is
GPLv3**, so an App Store submission cannot contain it, and native bash is
therefore on a clock regardless of anything technical.

That decides a question that was otherwise going to be argued on merit. Native
bash and native zsh both exist; only one needs to survive; the licence picks it.

## The decisions

1. **Native bash is not getting checkpoint support.** It is being removed, so
   the work would be thrown away.

   **Superseded 2026-09-11 in the part that mattered.** This used to mean a
   checkpoint REFUSED while native bash was on any task stack, which turned a
   bash login shell into "you cannot suspend at all". It no longer does: a
   native program with no `ckpt_dump` is saved and re-launched from its command
   line, and the save reports which programs those were. bash still does not
   describe itself and still comes back at a fresh prompt -- that part of the
   decision stands -- but it no longer costs the whole session.
2. **Native bash is removed in 556**, announced in the 555 release notes so the
   removal is not a surprise to anyone whose login shell it is.
3. **`/etc/passwd` entries naming native bash are converted to the guest's own
   bash automatically.** Nobody should be locked out by an upgrade, and a login
   shell that no longer exists is exactly that. The conversion is by path, not
   by uid: any entry, not just uid 1000.
4. **A native dash ships in 555**, reachable as both `/AOK/native/dash` and
   `/AOK/native/sh`.

## zsh is the shell that checkpoints, and it is already most of the way

Verified 2026-09-10 rather than assumed. `deps/zsh/Src/aok_fork.c` is **3222
lines against bash's 2093**, and it already does the thing a checkpoint needs:
hand a fresh native zsh the parent's full state.

zsh is the easier of the two for a structural reason. It has a **`-L`
convention** across its builtins meaning "output suitable for re-input" --
`alias -L`, `bindkey -L`, `zstyle -L`, `zmodload -L`. bash has no equivalent,
which is why its dump needed a hand-written `declare` emitter, a second pass for
readonly variables, and hand-ordered special traps.

Measured round trip into a fresh shell: variables with every attribute (export,
integer, array, associative, readonly), functions, aliases, options and traps
all restore. The existing code already solves the ordering traps that make this
hard -- modules read before `zsh/parameter` loads, `zstyle` emitted after the
options because it is an autoloaded builtin, traps distinguished by whether the
shell was handed them or set them itself, autoload flags, the user-populated
command hash, and history travelling as a file.

**The code has already named the boundary a checkpoint cares about**, which is
the most useful thing in it:

> `execpline`'s fork to continue a stopped pipeline is deliberately left
> refusing. Its child does not discard its state, it CARRIES ON with it, in the
> middle of a half-executed loop with a live C stack. That is the one thing a
> re-launch cannot express.

A re-launch expresses "state in, run this, status out". It cannot express
"resume from here". So a checkpoint takes the same line: **at the prompt is a
quiet point** -- the C stack holds only "waiting for input" and everything
semantic is what `aok_fork` already dumps -- and **mid-command is a refusal**,
exactly as `execpline` already refuses.

Two gaps, flagged as unverified rather than claimed: the **job table** (a
subshell does not need background jobs, a checkpoint does) and whether
**history-as-a-file** is the right shape when restoring hours later rather than
milliseconds.

## dash's licence is not clean, and this is the finding that qualifies the plan

**dash is BSD-3-Clause except for one file, and that file's OUTPUT is linked
into the binary.** From Debian's own `/usr/share/doc/dash/copyright`, read on
2026-09-10:

```
Files: src/mksignames.c
Copyright: 1992, 1996, 1997, 1999, 2000, 2002-2012, Free Software Foundation, Inc.
Comment: This file is not directly linked with dash.  However, its output is.
License: GPL-2+
```

Everything else is BSD-3-Clause, plus `src/bltin/test.c` in the public domain.
Debian flags the exception itself, in that `Comment:` field, precisely because
the distinction between "the tool is GPL" and "the tool's output is linked"
is the one that matters.

So "no licence issues with dash" is **not** accurate as stated, and shipping it
unexamined would carry the same class of problem bash is being removed for.

**And GPL-2+ is not materially safer than GPLv3 for this purpose**, which is
worth stating because the instinct is to assume it is. The App Store problem is
the VLC precedent, and VLC was pulled over **GPLv2**: the argument was that the
store's usage terms impose further restrictions, which GPLv2 section 6 forbids.
The `+` means "or later" and gives the recipient a choice of versions; it does
not soften the conflict.

**It is fixable, and cheaply.** `mksignames.c` is a build-time generator whose
entire output is a table of signal names and numbers -- `const char *signal_names[]`
and friends. That is derivable from the platform's own `signal.h` by a
generator written from scratch, and AOK has to supply signal names to native
programs anyway. The plan is therefore:

- do not build `mksignames.c` at all;
- generate the signal-name table with AOK's own generator;
- record in the build that this is deliberate, so nobody restores it later for
  convenience.

**And there is already the right place to record it.** `meson.build` prints a
`Licensing` summary at configure time -- "native bash: yes -- binary contains
GPLv3", "native zsh: yes -- permissive" -- written because "the alternative is
finding out from an App Store takedown". dash gets a line there too, and it has
to be the accurate one: permissive *provided* `mksignames.c` is not built. A
summary line that says so is much harder to lose than a comment in a build file.

**This is an engineering judgement about what to link, not legal advice**, and
the maintainer should confirm the conclusion before an App Store submission
depends on it.

### The exact replacement contract, read off Debian's tree (2026-09-10)

Verified against `salsa.debian.org/debian/dash`, branch `debian/unstable`, so
this is a spec rather than an estimate:

- `src/mksignames.c` opens *"Copyright (C) 1992 Free Software Foundation"* and
  *"You should have received a copy of the GNU General Public License along with
  **Bash**"*. It is literally bash's file, which is a tidy confirmation of why
  it is the one exception in an otherwise BSD tree.
- It builds one thing: `char *signal_names[2 * NSIG + 3]`, with `signal_names[0]
  = "EXIT"` and the rest indexed by signal number.
- Exactly two files consume it, each with a bare `extern char *signal_names[];`
  -- `src/jobs.c:251` and `src/trap.c:81`.
- The build wires it as `signames.c: mksignames` in `src/Makefile.am`, linked
  through `dash_LDADD = builtins.o init.o nodes.o signames.o syntax.o`.

So the replacement is: **emit a `char *signal_names[]` where index 0 is "EXIT"
and index N is signal N's name**, from the platform's `signal.h`. Nothing else
in dash touches it, and `mksignames.c` is then simply never compiled -- it is a
HELPER in `src/Makefile.am`, not a source file of the shell.

### Source: Debian's packaging (maintainer's call, 2026-09-10)

`https://salsa.debian.org/debian/dash.git`, branch `debian/unstable`. Herbert Xu
is both upstream and the Debian maintainer, so this is the maintained line
rather than a downstream patch pile, and `debian/*` is itself BSD-3-clause.

One wrinkle for the vendoring rule: Salsa is GitLab, so there is no GitHub
upstream to `gh repo fork`. `emkey1/dash` has to be created and seeded from
Salsa rather than forked, which still satisfies the rule's intent (a fork
submodule the project controls, never a snapshot or an untracked tree) but is
not the usual one-command path.

## Order of work

1. ~~Checkpoint refuses on native bash, and names it.~~ **DONE, then
   REVERSED 2026-09-11.** `/proc/ish/checkpoint` still names each native
   program on a stack and gives its verdict, but the verdict for one that
   cannot describe itself is now "re-launched from its command line", not
   "refused". Measured: with a native bash alive, a checkpoint writes a 4.5 MB
   image and the restore starts bash again (a marker file the child appends to
   holds two lines after the round trip, where the save used to be refused
   outright).
2. ~~`/etc/passwd` conversion.~~ **DONE** -- `convert_native_bash_shells` in
   `native-links.sh`, matching BOTH spellings (`/AOK/native/bash` and
   `/usr/local/native-bin/bash`) for EVERY entry rather than uid 1000. Verified:
   two non-1000 accounts converted, dry run previews without writing, line count
   checked, backup written, and the no-guest-bash fallback exercised by hiding
   every bash -- it chose `/bin/sh`, which runs. The default native shell also
   flipped from bash to zsh, since preferring bash would keep handing new
   installs the shell being removed.

   Worth noting what this found: `build/alpine-arm64-test` already had FOUR
   accounts pointing at `/AOK/native/bash`. The lockout is not hypothetical.
3. Native dash: `deps/dash` as a fork submodule (`emkey1/dash`, per the
   project's vendoring rule), built without `mksignames.c`, installed as both
   `dash` and `sh`.
4. 555 release notes: announce the 556 removal.
5. **556: `/AOK/native/bash` becomes a symlink to `/bin/bash`.** Maintainer's
   call, 2026-09-10. Removing the native binary leaves the PATH dangling for
   everything that is not a login shell -- a script with
   `#!/AOK/native/bash`, an exported `SHELL`, a Makefile, muscle memory -- and
   decision 3 above only rewrites `/etc/passwd`. A symlink turns every one of
   those into the guest's own bash instead of a "not found".

   Already established, so nobody re-derives it:

   - **aokfs serves symlinks already.** `fs/aok.c` has the machinery for
     `/AOK/fixes/debian` -> `devuan`: `aokfs_node_is_symlink`,
     `aokfs_symlink_target`, `S_IFLNK | 0777` in `aokfs_node_mode`, and
     `aokfs_readlink` wired into the mount ops. The change is a node kind, one
     `case` in the target switch, and dropping the node from
     `aokfs_node_is_native` -- because the point of 556 is that the interpreter
     is no longer compiled in.
   - **`/bin/bash` is the right target on all three roots.** Devuan and Alpine's
     `bash` package both install it there, and Arch has `/bin` -> `usr/bin`.
   - **It may dangle, and that is acceptable.** A root with no bash installed
     gives ENOENT, which is what the path gives today after removal anyway. The
     login-shell case does not rely on it: `convert_native_bash_shells` in
     `native-links.sh` already rewrites `/etc/passwd` and falls back to
     `/bin/sh` when there is no guest bash.
   - **Check it is not still dispatched.** `kernel/native.c` resolves
     `/AOK/native/*` before the filesystem answers; the symlink is only reached
     if the bash entry is gone from the native program table. That is the same
     removal 556 is doing, so it is one change, not two -- but it is the thing
     to verify rather than assume (see `syscall-table-entry-not-reachable`).
