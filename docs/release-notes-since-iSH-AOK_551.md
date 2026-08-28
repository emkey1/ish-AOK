# Release Notes Since `builds/iSH-AOK_550`

21 commits. A short cycle with one substantial new capability — FUSE — and a run
of work on the boundary between the guest and the app: what a mount can do, who
owns a file the GUI creates, and which operations an unprivileged session is
allowed to perform at all.

## Highlights

**FUSE.** `/dev/fuse` and a `fuse` filesystem type, speaking FUSE protocol 7.31.
A daemon you compile inside your root against the distro's own `libfuse` mounts
and serves a filesystem the same way it would on Linux — sshfs-style overlays,
archive mounts, in-memory scratch filesystems, without patching the daemon.
libfuse2 and libfuse3 both work. See `/AOK/docs/fuse.md` for the deliberate v1
limits: no `mmap`, so nothing executes from a FUSE mount, and no
`FUSE_INTERRUPT`, `FORGET`, readdirplus or splice.

**Bind mounts do what Linux's do.** `mount --bind` now binds a single *file*
over another file, not only directory over directory — the case most people
assume is missing, and the one that shadows a config file without touching the
original. Mixed shapes fail `ENOTDIR` and a source that does not exist fails
`ENOENT`, rather than quietly creating a bind that shadows its target with a
dead path. `--rbind` became genuinely recursive: it replicates every mount under
the source subtree at the matching place under the new location, where before
`MS_REC` was a stripped no-op.

**Apple Shortcuts.** A headless *Run Command* action executes a command in the
guest under the native zsh and returns its output to the shortcut, without the
app coming to the foreground, plus *Open iSH-AOK* destinations with Siri
phrases. See `/AOK/docs/shortcuts.md`.

**mount and chroot now require privilege, as they do on Linux.** Six doors into
the mount table — `mount`, `umount2`, `fsopen`, `fsmount`, `move_mount` and the
native shim's `nlibc_mount` — gate on `CAP_SYS_ADMIN`; `chroot` gates on
`CAP_SYS_CHROOT`. Previously an unprivileged process could do all of it. The
check is a new `current_capable()`, which honours a capability held without uid
0 rather than testing for root alone.

**This one changes behaviour you may rely on:** a session with *Open Everything
as Default User* enabled runs as uid 1000, so `mount` and `chroot` there now
fail with `EPERM` where they previously worked. That is the intent — it is what
Linux does — but anything of yours that mounted from such a session needs
`sudo` now.

## Crashes and hangs that reached users

**A fatal signal could fire while host stdio held a `FILE` lock**, abandoning the
lock and wedging the process. Fatal delivery and handler delivery are now
deferred while a thread is inside a host-stdio callback, with its own give-up
limit.

**Forking a task whose first exec was native crashed the kernel.** Fixed.

**The JVM did not start on aarch64** — HotSpot aborted during startup with
"Field too big for insn", blocking every JVM-based tool. AOK was reporting
`DCZID_EL0` with the DZP bit set, telling the guest that `DC ZVA` was
prohibited; HotSpot generated a block-zeroing stub with a zero ZVA length,
producing an all-ones mask the AArch64 logical-immediate encoding cannot
represent. `DCZID_EL0` now reads `0x4`, which is what a real core reports.
Fixed in 550 and confirmed closed this cycle
([#542](https://github.com/emkey1/ish-AOK/issues/542),
[#564](https://github.com/emkey1/ish-AOK/issues/564)).

**helix loaded on the first run only.** Fixed.

## Files the GUI creates

Three related fixes to ownership. Files created through the GUI now belong to
the default user when the preference asks for it, rather than to root; atomic
saves no longer hand the replaced file to root; and the LLM chat and Shortcuts
command surfaces honour *Open Everything as Default User* when choosing the
account a command runs as. The net effect is that a file you make in the app is
one you can still write from your shell.

## Elsewhere

**`stty` understands `ixon` and `-ixon`.** After `native-links.sh` puts
SmallCLUE's `stty` ahead of the distro's on `PATH`, shell startup files that
touch `stty` were answered with `unsupported argument` and exit 1 — zprezto runs
`stty -ixon` from its environment module, so every login printed an error under
the motd ([#561](https://github.com/emkey1/ish-AOK/issues/561)). The flag now
does the real thing through `tcgetattr`/`tcsetattr`, and a missing tty reports
itself but exits 0 rather than failing. Other stty arguments will still error;
report them and they can get the same treatment.

**`NO_COLOR` reaches the applets, not just `md`.** Eleven more places — `ps`,
`top`, `ls`, `df`, `cal`, `grep`, `watch`, the usage listing and `rm`'s
confirmation prompt — now share one colour policy, so `NO_COLOR=1`, `TERM=dumb`
and an unset `TERM` are honoured everywhere. `ls` and `grep` keep
`--color=never|always|auto` on top of it. Between them those places used to emit
2040 SGR sequences into output that had asked for none.

**`md`'s menus clear the screen again**, and the image viewer's zoom toggle
gained an accessibility label — it was the only control in that toolbar without
one ([#563](https://github.com/emkey1/ish-AOK/pull/563)).

**The Linux build was broken for a day.** A `bool` added to the force-included
native shim header without `<stdbool.h>` compiled on Darwin, where the SDK
supplies it transitively, and failed on every Linux build. The Mac build
structurally cannot catch that class.

## Documentation

`/AOK/docs/fuse.md` is new — using FUSE, the v1 gaps, and the ownership trap
that makes an unprivileged mount look broken when the daemon reports its files
as root-owned.

`roots.md` gains a section on bind mounts by hand, including the consequence
that with no mount namespaces a bind is visible system-wide rather than private.
`native-setup.md` gains a *Colour* section documenting the policy above.
`proc-ish.md` had been naming a Settings switch that no longer exists: it is
**Open Everything as Default User** now, and the `/proc/ish` key stays
`login_as_default_user` — the one place the doc's own naming rule does not hold.
All three READMEs carry a FUSE entry.

## Validation

- The guest regression suite on an M4 iPad Pro: **118 passed**, one
  environment-dependent failure (`inaddr_any_iface` reaches the listener on
  loopback, USB and Tailscale but times out on the cellular interface).
- Privileged tests now run under `sudo` rather than skipping themselves. Eight
  of them had been silently retired on any session running as the default user,
  while the suite still reported a clean pass.
- Bind-mount and rbind behaviour verified in a live guest against Linux
  semantics: file-over-file, both mixed shapes, missing source, recursive
  replication and the self-clone exclusion.
- The privilege gates verified both directions — root still mounts and chroots;
  uid 1000 gets `EPERM`.
- GitHub CI green on Linux/clang, Linux/gcc and macOS.

## Known gaps

- A kernel-wide conformance audit against real Linux ran at the end of this
  cycle and confirmed a number of divergences that are **not** fixed here,
  including four rated critical. They are long-standing rather than new — all
  four predate 550 — so this release neither introduces nor worsens them, and
  they are the subject of the next cycle.
- `inaddr_any_iface` fails on a cellular interface, as described above.
- Two guest tests guard on `geteuid()` and so cannot run unprivileged; the
  kernel side that made those guards necessary is now correct, but the tests
  have not been revisited.

## Commit Range

`builds/iSH-AOK_550..builds/iSH-AOK_551` — 21 commits.
