# Native mode (build 558): an iSH-AOK session with no Linux distribution

Status: **plan**, 2026-10-02. Nothing here is built yet.

Decided by the maintainer, 2026-10-02:
- Native is **bundled** (an Official Distributions entry beside Alpine and
  Devuan, nothing to download), but **not preselected** on first launch.
- **SmallCLUE `init` is pid 1 from the start**, with a simple `/etc/rc`
  system (step 4a).
- **Every applet gap in step 5 ships in 558**, mostly in SmallCLUE.
- A native Python with `pip` is a future TODO (docs/TODO.md), not 558.
- **The uid-1000 user's name is asked for at the first start of a native
  root**, and the root honours **"Open Everything as Default User"** exactly as
  a distro root does (step 4b).

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
| `/bin/login -f root` is the default launch command (UserPreferences.m:1292), and "Open Everything as Default User" works by rewriting it to `-f <uid-1000 name>`; `ISHSessionCommandWithFallback` knows only guest paths | app | Steps 4, 4b, 5: SmallCLUE `login` at `/bin/login` |
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
5. **pid 1 is SmallCLUE `init`** (decided 2026-10-02). It is reached through
   `/sbin/init -> /AOK/native/smallclue`, so the default Boot Command
   (`/sbin/init`) needs no change, and every terminal is a normal pty session
   as on a distro root. Step 4a covers what `init` lacks today.

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
- `/sbin/init`, `/sbin/halt`, `/sbin/reboot`, `/sbin/poweroff` and `/usr/bin/sv`
  -> `/AOK/native/smallclue`. `native-links.sh` keeps excluding them for distro
  roots, where the distro's own init owns pid 1.
- `/etc/rc`, `/etc/rc.shutdown`, an empty `/etc/rc.d` and `/etc/service`
  (step 4a), written only if missing.
- `/bin/login` and `/usr/bin/login` -> `/AOK/native/smallclue` (step 5.3), so
  the default launch command, `/bin/login -f root`, works unchanged.
- `/bin/sh -> /AOK/native/sh`, `/bin/dash`, `/bin/zsh`, and
  `/bin/su`, `/usr/bin/sudo`, `/usr/bin/passwd`.
- One link per working applet in `/usr/bin`, from the shared manifest
  (decision 4), plus the standalone programs (`hx`, `motepad`, `ktop`,
  `wl-present`, `bmm`, `bmt`). `/usr/bin/env` matters, because `#!/usr/bin/env`
  is everywhere.
- **Written only if missing**, so they are the user's after first boot:
  `/etc/passwd`, `/etc/group`, `/etc/shadow` (0600) and `/etc/sudoers` (0440).
  They hold root only, with shell `/AOK/native/zsh`, groups `wheel` and
  `sudo`, and `%sudo ALL=(ALL) ALL`. The uid-1000 user is added later, from
  the first-start prompt (step 4b), by `native_root_add_user()` in the same
  file, which the CLI reaches through `ISH_NATIVE_USER=<name>`. Also
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
  - boot `/sbin/init` as usual. It is SmallCLUE `init` (decision 5), so
    `BootCommandWithInitFallback` never takes the console-only fake-init path.
- **Sessions**: no app change. The launch command stays `/bin/login -f root`,
  and `/bin/login` is SmallCLUE's `login` (step 5.3). The existing default-user
  substitution therefore applies as-is (step 4b). Separately, and good for
  every root: add `/AOK/native/zsh` and `/AOK/native/sh` as the last entries in
  `ISHSessionCommandWithFallback`, so a broken distro root still gives you a
  shell.
- **The `/bin/sh`/`/bin/su` callers** (run_guest_command_capture, Display, LLM
  tools, Shortcuts) need no change, because step 2 creates those paths. Display's
  Wayland session cannot work without a distro: grey it out with a "needs a
  distribution" note rather than letting it fail.
- **Checkpoint/suspend**: the root identity works as-is. zsh restores its state
  through `ckpt_dump`, dash restarts, and `init` needs the restore rule in
  step 4a.

### 4a. `init` and `/etc/rc` (SmallCLUE)

SmallCLUE already has most of this (deps/smallclue/src/core.c:22079). `init`
runs `/etc/rc` through `smallclueSpawn`, which was rewritten precisely because
AOK has no `fork()`. It also reaps orphans with `waitpid(-1)` while rc runs.
`runit` starts every `/etc/service/*/run`. Four things do not fit AOK yet:

1. **`init` returns.** When rc exits, a pid-1 `init` does `kill(-1)` and
   returns. In AOK, pid 1 exiting halts the guest (kernel/exit.c:1214, and the
   app shows "System Halted", per the #587 fix 25e4591d). That shape suits
   PSCAL's root, whose rc *is* the session (it ends by running exsh). It is
   wrong here, so change it to the sysvinit shape: run rc to completion, then
   reap forever.
2. **There is no shutdown path.** `halt`/`reboot`/`poweroff` just `exit(0)`
   themselves (core.c:22380). Use busybox's convention, which Alpine's init
   already follows in AOK: the applets signal pid 1 (`halt` USR1, `poweroff`
   USR2, `reboot` TERM). `init` then runs `/etc/rc.shutdown`, does
   `kill(-1, TERM)`, waits up to 3 s, sends `kill(-1, KILL)` and exits, and the
   app's existing halt handling takes over. Ctrl-Alt-Del (SIGINT) does nothing.
3. **A restore must not re-run rc.** `init` has no `ckpt_dump`, so a restore
   re-launches it from its argv (`ckpt_dispatch_native`), and every service
   would start twice beside its restored self. Have `ckpt_dispatch_native` set
   `AOK_NATIVE_RESTORED=1` in a re-launched program's environment, which is
   useful to any native program, and have `init` skip rc and go straight to
   reaping when it sees that.
4. **`runit` does not supervise.** It starts each service once, and one that
   dies stays dead. Add restart with backoff (1 s doubling to 60 s), honour a
   `down` file, and add an `sv` applet (`status`/`up`/`down`/`restart`/`stop`)
   that talks to it through `/run/service/<name>/` (pid, state, a control
   FIFO). `runit` stays a plain process started by rc, not pid 1.

**The rc scheme**, kept deliberately small: these are provisioned shell
scripts, written only if missing so they are the user's to edit, and run by
native dash.

- `/etc/rc`: runs every executable `/etc/rc.d/S??*` with `start`, in lexical
  order; starts `runit /etc/service &` if that directory has entries; runs
  `/etc/rc.local` if it is executable; then exits 0. One failing script is
  logged to `/dev/kmsg` and does not stop the rest.
- `/etc/rc.shutdown`: `sv stop` for everything, then the same `S??*` scripts
  with `stop`, in reverse order.
- Nothing is enabled by default. The hostname, `/etc/hosts` and DNS stay the
  app's job, as they are on distro roots. The first real consumer is a native
  sshd (native-sshd-plan) when it lands. A commented example lives at
  `/etc/rc.d/S50example.disabled`.

**Where:** SmallCLUE upstream (emkey1/smallclue, landing on `main`), then a
`deps/smallclue` bump. The `AOK_NATIVE_RESTORED` half is in kernel/checkpoint.c.
Check that the app learns when a terminal session ends while `init` reaps it:
sessions are children of pid 1 (`become_new_init_child`), the same as under a
distro's init, so this should already hold. Confirm it in the spike.

### 4b. The first-start user prompt and "Open Everything as Default User"

**The prompt.** The first time a native root boots, the app asks for the name
of the everyday account. It asks at boot rather than at import, because a root
can also be installed headlessly with `manage-roots.sh`. "First time" means
`/etc/passwd` has no uid 1000 and there is no `/etc/aok-native-user-skipped`
marker.

- The sheet has a name field, validated as `[a-z_][a-z0-9_-]{0,31}` and not
  an existing account, and an optional password with confirmation. It offers
  **Create** and **Skip (root only)**.
- **Create** calls `native_root_add_user(name, hash)`. That writes the
  `/etc/passwd`, `/etc/group` and `/etc/shadow` lines (uid/gid 1000, groups
  `users` and `sudo`, shell `/AOK/native/zsh`), creates `/home/<name>` 0700
  owned by the user, and seeds it with `.zshrc` from `/etc/skel` if present. A
  password is hashed `$6$` with kernel/sha_crypt.c. With no password the shadow
  field is `!`: the account still opens through `login -f`, but `sudo` needs a
  password set first (`passwd` from a root Session Shell).
- **Skip** writes the marker. The root then has root only, exactly like a
  distro root with no uid 1000, so the setting below simply has no one to
  switch to.
- **Nothing waits for the sheet.** Init and `/etc/rc` boot while it is up,
  and the root Session Shell is available at once, as it always is (see below).
  A Workspace terminal opened before the account exists opens as root, which
  is what the substitution already does when there is no uid 1000. So a slow
  answer, or none, can neither hold the boot nor trip the launch watchdog.
- Headless: `manage-roots.sh install aoknative --user NAME`, carried to the
  boot as a one-shot plist field, and `ISH_NATIVE_USER` in the CLI.

**"Open Everything as Default User"** (`shouldLoginAsDefaultUser`, key
"Login As Default User") needs no native-specific code once the account
exists and `/bin/login` is SmallCLUE's:

- Workspace terminals: `ISHCommandWithDefaultUserSubstitution`
  (TerminalViewController.m:92) rewrites `/bin/login -f root` to
  `/bin/login -f <name>`, using `+defaultUserAccountName`, which reads
  `/etc/passwd`. Session Shell windows stay root (`alwaysLoginAsRoot`), as
  they do on a distro root.
- The other consumers already key off the same lookup and `/bin/su`, which
  step 2 links to native su: AppDelegate.m:2899/2906, Display
  (DisplayViewController.m:112), LLM tools (LLMChatTools.m:146), and
  `run_guest_command_capture`'s `/bin/su - user`.
- **The root Session Shell is always there.** The terminals already guarantee
  a root prompt through the Session Shell (`alwaysLoginAsRoot`;
  WorkspaceViewController.m:4240), and native mode must not weaken that.
  `/bin/login -f root` must work whatever root's shadow field says (`-f`
  skips authentication). The session fallback chain must end in
  `/AOK/native/zsh`, which is native and needs no root file at all (step 4).
  That way a user who breaks `/bin/login`, `/etc/passwd` or their own account
  still gets a root prompt to repair it from. Root's shell is not taken from
  `/etc/passwd` when that shell does not exist: `login` falls back to
  `/AOK/native/zsh`.
- What must hold for this to work: SmallCLUE `login -f <name>`, run by root,
  sets uid/gid/supplementary groups, `HOME`, `SHELL`, `USER`, `LOGNAME` and the
  cwd from `/etc/passwd`, then runs the shell as a login shell (`-zsh`). Since
  a native exec is spawn-then-wait, `login` stays as the session leader and the
  shell must become the terminal's foreground process group. Ctrl-C, Ctrl-Z
  and `fg` under `login` are spike items (step 1).

### 5. Applet gaps, all for 558 (SmallCLUE)

All of these ship in 558 (decided 2026-10-02). They are ordered by how soon a
person hits them:

1. `reset` and `tput` (`clear`, `cols`/`lines`, `setaf`/`setab`/`sgr0`,
   `bold`, `cup`, `civis`/`cnorm`, `smcup`/`rmcup`). Scripts and prompts use
   them, and the terminfo from step 3 is all they need. Golden-test them against
   ncurses' `tput` from the Devuan root.
2. `free`, reading `/proc/meminfo`, with procps' column layout and `-h`/`-m`/`-g`.
3. `login`, which native mode's sessions depend on (step 4b). It supports
   `-f <user>` (pre-authenticated, root only, as util-linux and busybox do) and
   a plain `login [user]` that prompts for a password and checks it against
   `/etc/shadow` with the existing `$5$`/`$6$` code (kernel/sha_crypt.c). It is
   not setuid: like Linux's, it is only useful when run as root. Golden-test it
   against busybox `login -f` on Alpine (environment, groups, cwd, argv[0]
   `-zsh`).
4. Native `mount`, `umount` and `chroot`. Their real bodies sit behind
   `#if __linux__`. Route `mount(2)`, `umount2(2)` and `chroot(2)` through the
   shim's syscall path (kernel/native_libc.c) and build those bodies for AOK.
   This is what lets native mode reach a distro installed alongside it:
   `mount-root.sh <root>` must then work in native mode, so test it there too.
   Remove them from `native-links.sh`'s EXCLUDED list once they pass.
5. `halt`/`reboot`/`poweroff` signalling pid 1, and `sv`: see step 4a.

Each gets golden cases in `/AOK/tests` the way `native_coreutils.c` does, and
each lands on SmallCLUE upstream first.

Out of scope: a package manager. A native Python, which would bring `pip`, is
queued in docs/TODO.md as a possible future feature. Native mode's answer to "I need X" is
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
  `login -f <user>` gives the right `id`, `HOME`, cwd and `$0`, and job control
  works under it; a root prompt still comes up with `/bin/login` removed, with
  root's passwd shell pointing at a missing file, and with root's shadow field
  locked;
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
  Native → user prompt → prompt), with "Open Everything as Default User" both
  off and on (a Workspace terminal opens as that user, while the Session Shell
  stays root), and Skip. The Session Shell must give a root prompt while the
  user sheet is still up. Then the device leg booting it with `ISH_BOOT_ROOT`.
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
| 4a init, rc, runit/sv, restore rule | 2 days | 6 |
| 4b first-start user prompt, add_user | 1 day | 6 (needs 5.3 login) |
| 5 reset/tput/free/login/mount/umount/chroot | 3–4 days | 6 |
| 6 tests and gate leg | 1–2 days | release |
| 7 docs | 0.5 day | release |

Steps 2, 3, 4a and 5 can run in parallel after the spike; 4a and 5 are
almost entirely SmallCLUE. The total is about 13–17 days.

## Decisions log

All answered by the maintainer, 2026-10-02:

- Native is bundled but not preselected.
- SmallCLUE `init` with `/etc/rc` from day one.
- All of step 5 is in 558.
- Python/pip comes later.
- The uid-1000 name is prompted for at first start, and "Open Everything as
  Default User" is honoured.
- The root Session Shell is always available, including while the first-start
  sheet is up and when the root's own login files are broken.

No open questions remain. Revisit after the step-1 spike.
