# Native mode (build 558): an iSH-AOK session with no Linux distribution

Status: **plan**, 2026-10-02. Nothing here is built yet.

## The idea in one paragraph

Today every session boots a distro root (Alpine, Devuan, ...) and the native
programs in `/AOK/native` are an add-on you link in with `native-links.sh`.
Native mode reverses that: the root is a tiny, AOK-generated filesystem whose
`/bin` and `/usr/bin` point straight into `/AOK/native`, and the login shell is
native zsh. Nothing is downloaded, nothing is emulated on the common path, and
first launch is instant. It is offered as one more root in the Filesystems
screen and the first-launch picker ("iSH-AOK Native: no distribution"), so
switching between it and a distro uses the machinery that already exists.

## What already works (measured by reading the code, 2026-10-02)

- **A task whose first image is native.** `task_run_current` runs
  `native_exec_run_pending()` before any CPU/TLB setup (kernel/task.c:1817).
  The app's boot `do_execve` + `task_start` already reaches it.
- **A pid 1 that never loads an image.** The fake-init supervisor
  (`StartFallbackConsoleSupervisor`, app/AppDelegate.m:1276) is a host thread
  that impersonates pid 1, forks console shells and reaps them.
- **Relaunch needs only `/AOK`.** zsh and dash re-spawn `/AOK/native/zsh` and
  `/AOK/native/dash` by absolute path (deps/zsh/Src/aok_fork.c:78,
  deps/dash/src/aok_fork.c:116). No root file is involved.
- **Symlinks dispatch.** `native_dispatch_exec` keys off the resolved fd being
  on aokfs (kernel/exec.c:2452), so `/bin/sh -> /AOK/native/sh` runs native dash.
- **An empty fakefs root can be made at runtime.** `fakefs_init_empty()`
  (tools/fakefs.c:355) already builds `/AOK/fakefs` at every boot
  (AppDelegate.m:4148). Its output passes `RootURLLooksValid`.
- **No host-data gaps for locale, TLS or DNS.** Locale data comes from the host
  (`nlibc_setlocale`). curl and wget use NSURLSession and the iOS trust store, so
  no CA bundle is needed. The app already writes `/etc/hosts`, `/etc/hostname`
  and `/etc/resolv.conf`.
- **Roughly 150 SmallCLUE applets**, including ls/cp/mv/sed/grep/awk/find/tar/
  gzip/less/ps/top/kill/df/dmesg/curl/wget/git/ssh/scp/sftp/vi, plus zsh, dash,
  hx, motepad, ktop, md and su/sudo/passwd.

## What breaks with nothing underneath

| Gap | Where | Fix in this plan |
|---|---|---|
| `/bin/sh` is hardcoded: ENOEXEC fallback in zsh/dash, `nlibc_system`, the aokfs stub, `run_guest_command_capture` (kernel/init.c:500-566), Display, LLM tools | many | Step 2 creates `/bin/sh`, so they work unchanged |
| `/bin/login -f root` is the default launch command (UserPreferences.m:1292); `ISHSessionCommandWithFallback` knows only guest paths | app | Step 4 |
| Fake-init means "one console shell, no pty sessions" (TerminalViewController.m:2023) | app | Step 4: a new pid-1 mode |
| No `/etc/passwd`: ssh exits with "No user exists for uid"; su/sudo need passwd/shadow/sudoers | native_libc.c:7102 | Step 2 |
| No terminfo: zsh prints "can't find terminal definition" and ZLE degrades. The app sets `TERM=screen-256color` | native_termcap.c:930 | Step 3 |
| No zoneinfo: the localtime writer links `/usr/share/zoneinfo/<zone>` only if that file exists (AppDelegate.m:1772) | app | Step 3 |
| No zsh function tree, so `compinit` fails | zsh_glue.c:119 | Step 3 |
| Init ABI defaults to i386 (task.c:1085), so `uname -m` reports `i686` | kernel | Step 4 |
| `FsInitialize` touches `/etc/apk/repositories` and `/bin/login` (CurrentRoot.m:92-212) | app | Step 4: skip for native roots |
| No `reset`, `tput`, `free`, `login`, `man`. `mount`, `umount`, `chroot` and `passwd` are Linux-gated stubs (native-links.sh:200-219) | smallclue | Step 5 |

## Design decisions (recommendations; the open questions are at the end)

1. **The native root is a real, persistent fakefs**, not tmpfs. Home
   directories, `~/.ssh`, history and dotfiles must survive a relaunch.
   Checkpoint/suspend already keys on a root identity (AppDelegate.m:3030), the
   Files app and FileProvider already understand a fakefs root, and delete,
   rename and `/AOK/roots` exposure all keep working. Ship no special case where
   a plist field does the job.
2. **It is a catalog entry, not a sentinel.** Add a `kind: "native"` choice to
   `BuildRootChoices` (app/Roots.m:279) with id `aoknative`, guestABI `arm64` and
   initialWindow `session-shell`. "Importing" it calls `fakefs_init_empty()` and
   then the provisioner, with no archive. `ish-root.plist` records
   `kind=native`. `manage-roots.sh install aoknative`, `op=default`,
   `ISH_BOOT_ROOT` and the Filesystems screen then all work with no new verbs.
3. **The skeleton is re-provisioned at every boot**, idempotently, by C code
   shared between the app and the CLI. An app update that adds or removes a
   native program then updates `/usr/bin` on the next launch, and a link the
   user replaced with a file of their own is never touched. The `--force`/owned-
   link rules are the same ones `native-links.sh` already follows.
4. **One list of what to link.** `native-links.sh` carries the measured
   EXCLUDED/PROBED applet lists today. Move them into a manifest that both the
   script and the provisioner read, so native mode and a distro with links can
   never disagree about which applets work.
5. **pid 1 stays host-side.** Use a fake-init variant that only reaps and
   respawns nothing, with every terminal a normal pty session. SmallCLUE's
   `init` + `/etc/rc` is a later option for people who want services. It is not
   needed to ship.

## Steps

### 1. Spike in the CLI (half a day; do this first)

Prove the assumptions before building. Make an empty fakefs with
`fakefsify` from an empty tar, hand-create `/bin/sh`, `/etc/passwd` and `/tmp`,
then run `./build/ish -f build/native-test /AOK/native/zsh -l`. Exercise
subshells, pipelines, `$(...)`, an ENOEXEC script, `ssh -G`, `git --version`,
`curl https://...`, `sudo -l`, `su`, Ctrl-C and job control. Write down every
failure, then adjust the steps below before starting them.

### 2. `kernel/native_root.c`: the provisioner

Add `int native_root_provision(const struct native_root_opts *)`, written
against `generic_*` calls so it runs identically in the app and the CLI. It
creates:

- Directories: `/bin /sbin /usr/bin /usr/sbin /usr/local/bin /etc /etc/profile.d
  /root /home /tmp /var/tmp /run /var /dev /proc /sys`. Fold in
  `FakeInitPrepareGuestRoot` (AppDelegate.m:1149) rather than duplicating it.
- `/bin/sh -> /AOK/native/sh`, `/bin/dash`, `/bin/zsh`, and
  `/bin/su`, `/usr/bin/sudo`, `/usr/bin/passwd`.
- One link per working applet in `/usr/bin`, from the shared manifest
  (decision 4), plus the standalone programs (`hx`, `motepad`, `ktop`,
  `wl-present`, `bmm`, `bmt`). `/usr/bin/env` matters, because `#!/usr/bin/env`
  is everywhere.
- **Written only if missing**, so they are the user's after first boot:
  `/etc/passwd`, `/etc/group`, `/etc/shadow` (0600) and `/etc/sudoers` (0440).
  These hold root plus one uid-1000 user (see Q1) in groups `wheel` and `sudo`,
  with `%sudo ALL=(ALL) ALL` and both shells set to `/AOK/native/zsh`. Also
  `/etc/shells`, `/etc/profile` (PATH with `/AOK/persist/bin` first, matching
  `BootEnvironmentForCommand`), `/etc/zshrc` (a usable prompt and
  `compinit`) and `/etc/os-release` (`ID=aok-native`, `NAME="iSH-AOK Native"`,
  `VERSION_ID=<build>`).
- **Always rewritten** (AOK-owned): `/etc/aok-native.version`, the record of
  which links the provisioner made, so the next boot can remove links it no
  longer wants.

The CLI gets `ISH_NATIVE_ROOT=1` (or `-N`) to run the provisioner after
mounting, so the gate can build `build/native-test` in one command.

### 3. Data the root would otherwise get from a distro, served from aokfs

All of these ship read-only in the app bundle and are reached by a symlink or
search path the provisioner sets:

- **terminfo**: compiled entries for `screen-256color`, `xterm-256color`,
  `xterm`, `tmux-256color`, `vt100`, `vt220`, `linux` and `dumb`, built at build
  time with the host `tic` from ncurses' `terminfo.src` (MIT-X11 licence). They
  are served at `/AOK/share/terminfo`, with `/usr/share/terminfo -> /AOK/share/terminfo`.
  Check that Backspace and arrows in zsh match a distro session: this is the
  class behind the earlier "backspace moves forward" bug.
- **zoneinfo**: preferred is a read-only realfs mount of the host's own
  `/usr/share/zoneinfo` at `/usr/share/zoneinfo`, since TZif is the same format,
  it costs nothing to bundle and it follows iOS tzdata updates. **Verify on
  device that the sandbox can read it.** The fallback is to bundle tzdata
  (public domain) into aokfs. After either one, the existing localtime writer
  needs no change.
- **zsh functions**: install deps/zsh's `Functions/` and `Completion/` trees
  (zsh licence, permissive) at `/AOK/share/zsh/functions`, and add that path to
  the `zsh_glue.c` fpath probe list.
- **Docs**: `md` already reads `/AOK/docs`. Point `/usr/share/doc/aok` at it so
  there is somewhere obvious to look in place of `man`.

Record each in `docs/CREDITS-aarch64.md` / the licences screen. None is GPL.

### 4. App integration

- **Roots.m**: add the catalog entry and its importer (decision 2), list it
  first in the first-launch picker, and label it "No download · 0 MB · runs
  only built-in programs". `RootsTableViewController` needs no special cases
  beyond the subtitle.
- **AppDelegate `-boot`**: when the booted root's plist says `kind=native`:
  - run `native_root_provision()` after `mount_root` and before
    `become_first_process`;
  - set `current->abi = GUEST_ABI_ARM64` (the host's architecture, so
    `uname -m` says `aarch64` and the shim takes its 64-bit struct layouts
    consistently);
  - skip `FsInitialize`'s apk and login work;
  - start pid 1 as the new "reaper" fake-init (decision 5) instead of
    `/sbin/init`.
- **Sessions**: for a native root the launch command is
  `/AOK/native/zsh -l`. The login step that `/bin/login -f` does today
  (uid/gid/groups, HOME, SHELL, USER, LOGNAME, cwd, from `/etc/passwd`) moves
  into a small C helper called from `become_new_init_child`, rather than
  depending on a `login` binary. `ISHCommandWithDefaultUserSubstitution` then
  uses that helper too. Separately, and good for every root: add `/AOK/native/zsh`
  and `/AOK/native/sh` as the last entries in `ISHSessionCommandWithFallback`,
  so a broken distro root still gives you a shell.
- **The `/bin/sh`/`/bin/su` callers** (run_guest_command_capture, Display, LLM
  tools, Shortcuts) need no change, because step 2 creates those paths. Display's
  Wayland session cannot work without a distro: grey it out with a "needs a
  distribution" note rather than letting it fail.
- **Checkpoint/suspend**: the root identity works as-is. Verify that the reaper
  pid 1 is saved and restored. zsh restores its state through `ckpt_dump`, and
  dash restarts.

### 5. Applet gaps worth closing for 558

Ranked by how soon a person hits them:

1. `reset` and `tput` (clear, cols/lines, setaf/sgr0). Scripts and prompts use
   them, and the terminfo from step 3 is all they need.
2. `free`, reading `/proc/meminfo`.
3. `login` as an applet: the step-4 helper with a password prompt, for
   `ssh`-less "switch user" flows. `su -` mostly covers it, so this is optional.
4. Make `mount`, `umount` and `chroot` work natively by routing them through
   the shim's syscall path instead of `#if __linux__`. This is what lets native
   mode reach a distro installed alongside (`mount-root.sh <root>`). It is
   valuable, but it can be 559.

Out of scope: a package manager. Native mode's answer to "I need X" is
"install a distro root next to it", or drop a static guest ELF into
`/AOK/persist/bin`, which still runs under emulation in native mode.

### 6. Tests

- **A gate leg**: add `native` to `LEG_NAMES` in tools/run-guest-gate.sh. It
  builds `build/native-test` with the CLI provisioner and runs a shell suite
  under `/AOK/native/sh`, since there is no `cc`. Keep it out of the
  never-ran-skip aggregation, as the unpriv and device legs are.
- **`tests/manual/native_mode.sh`** (new; register it in the three places, see
  fs/aok-tests.manifest): boot reaches a prompt; `id`/`whoami` show the
  passwd names; `uname -m` is `aarch64`; `/bin/sh -c` and an ENOEXEC script;
  subshell, pipeline and `$(...)` state; ssh/scp to localhost (needs a
  `sshd`-less target, so use `ssh -G` plus a loopback to a distro leg if one is
  up); `curl -sI https://...`; `sudo -n true` as uid 1000 with sudoers; `su -`;
  TZ shows the device zone; `tput cols`; and a re-provision that adds a missing
  link and leaves a user file alone.
- **The existing native suites** (`native_zsh_fork_state.sh`,
  `native_stdio_redirect.sh`, `native_signal_write_eintr.sh`,
  `native_sudo_sudoers.sh`) run in this leg as they are.
- **The golden C tests** (`native_coreutils.c` etc.) need a compiler. Build them
  in the arm64 Alpine leg, push them to the regress cache, and run the cached
  binaries in the native leg. They are static guest ELFs, which native mode
  runs fine.
- **Xcode is the only build**: Debug on the simulator (first-launch picker →
  Native → prompt), then the device leg booting it with `ISH_BOOT_ROOT`.
  Include a suspend/restore cycle and an app update over an existing native
  root, to check the re-provision.

### 7. Docs and release

- New `opt/AOK/docs/native-mode.md`: what it is, what is in the box, what is
  not (no package manager, no Wayland), and how to add a distro alongside.
- Update `roots.md` (the new catalog entry), `native-programs.md`, and
  `native-setup.md` (`native-links.sh` is unnecessary in native mode and refuses
  to run there).
- Add a 558 release-notes entry, and a `docs/TODO.md` pointer to this plan.

## Order and rough size

| Step | Size | Blocks |
|---|---|---|
| 1 spike | 0.5 day | everything (it may change the plan) |
| 2 provisioner + manifest | 1–2 days | 4, 6 |
| 3 terminfo/zoneinfo/fpath | 1 day | 6 |
| 4 app integration | 2–3 days | 6 (device) |
| 5 reset/tput/free | 1 day | — |
| 6 tests and gate leg | 1–2 days | release |
| 7 docs | 0.5 day | release |

Steps 2 and 3 can run in parallel after the spike. Step 5.4
(mount/chroot) is the natural first item for 559.

## Open questions (the user's call)

1. **Who is the user in a fresh native root?** The recommendation is root plus a
   uid-1000 account named after the app's existing default-user preference
   (falling back to `user`), with no password set. Sessions open as root, as
   today. sudo works for that user via `%sudo` after they `passwd` themselves.
2. **Should Native be preselected on first launch,** or listed first beside
   Alpine and Devuan with nothing preselected? It is the fastest start, but it
   cannot install packages.
3. **pid 1**: is the host reaper enough for 558, or do you want SmallCLUE
   `init` + `/etc/rc` (services, for example a future native sshd) from day one?
4. **Applet gaps**: are `reset`/`tput`/`free` enough for 558, or should native
   `mount`/`chroot` (reaching other roots) be in 558 too?
