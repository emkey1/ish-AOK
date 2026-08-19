# Release Notes Since `builds/iSH-AOK_548`

157 commits, and one of them is the release: iSH-AOK can now run programs that
were **compiled into the app** instead of translating them instruction by
instruction. A native program is host code running on a guest task's thread, so
it executes at full speed — but it has to be made to believe it is inside the
guest, and almost every commit here is some part of that belief being repaired.

The arc runs from a single applet through SmallCLUE's whole toolbox, an editor,
OpenSSH, bash, and finally zsh. The recurring lesson, learned in about thirty
different disguises: **the test is not "is this function pure?" but "can this
function's answer differ between the host and the guest?"** `getenv`, `getpwuid`,
`gethostbyname`, `setlocale`, `uname`, `sysctl`, `isatty`, `tcgetattr` — every one
of them answered about an iPhone until it was made to answer about the rootfs.

## Highlights

**Three crashes that reached users are fixed.**

*Every device below iOS 18.4 died the moment the terminal started.* `strchrnul`
is a GNU extension; Darwin grew one, but the SDK annotates it
`__IOS_AVAILABLE(18.4)` and this app deploys to 15.0 -- so the call compiled to
a WEAK import that is NULL on older systems, and calling it branched to address
zero. Reported on an A10X iPad Pro on 17.7.11, sourcing bash-completion. The
same binary is fine from 18.4 up, which is exactly what makes this the kind of
bug that ships: it cannot be reproduced on a current device. Answered in the
shim now, so nothing reaches libSystem for it (`63ad0e831`). All 455 imported
symbols were checked against the SDK's availability annotations; it was the only
one.

*Every crash report against build 548 was one bug.* `mknod(path, 0644, 0)` -- a
mode with no type bits -- is a normal way to create a regular file, and Linux
says so explicitly. We passed the bare mode through, built a tmpfs inode of no
type at all, and the first `write()` to it hit an assert and took the whole app
down, every guest process in it. Fixed at the one funnel every mknod variant
goes through, and the assert is now an errno: a guest-triggerable condition
should never abort a process shared by every task (`11edc1843`).

*`lsof` segfaulted, and took its whole listing with it.* Every anon_inode fd --
eventpoll, inotify, timerfd, signalfd, eventfd, pidfd -- reported `st_mode == 0`,
a stat with no file-type bits, which is not a thing a file can be. Linux gives
these `S_IFREG|0600`. Same commit fixes `anon_inode:[[pidfd]]`, which had picked
up a second set of brackets (`55d53227b`). The `/proc/net` v6 socket tables also
printed the IPv4 header, so `lsof` rejected both outright -- the rows were always
right, only the banner lied about them (`452406259`).

**Programs compiled into iSH-AOK now run as themselves.** `execve` of a path
under `/AOK/native` dispatches to a function inside the app rather than loading a
guest image, and the caller cannot tell the difference. `3f9923a0d`, with the
execve lifetime fix in `1a01a3a7d` — the program has to run *after* execve frees
its buffers, not instead of returning.

**SmallCLUE's applets are native.** The full busybox-style toolbox is compiled in
(`f8152644f`, `622ab23f8`), reachable through a link farm off `PATH`
(`0c7de3507`, `c36592960`). `tools/native-links.sh` also switches the UID 1000
login shell (`34f066e2d`).

**vi is a real editor.** Nextvi is compiled in rather than stubbed (`478398189`),
runs more than once and can shell out (`158127f6c`), and `!!` works through the
spawn seam (`2af64f723`). `cw` at the end of a line no longer swallows the
newline and joins the line below (`3f62c273c`).

**ssh, scp, sftp and ssh-keygen are real programs.** OpenSSH is carried as a
proper fork-and-submodule rather than a vendored snapshot (`81686796e`,
`1d9e3a5fd`), builds into the iOS app (`6e348924a`), prompts for a password
(`e3dc52826`), resolves names from the guest's own files (`3fe963f7b`), and
`ssh-copy-id` reports what it did (`8d57bc512`).

**bash runs natively, and it forks.** This is the part that looked impossible: a
native program is a C function on a thread inside one address space, so `fork()`
cannot copy it. Instead the shell **serialises its own state into a script and
re-launches itself** (`f4d981ce2`). Subshells, pipelines, command substitution and
process substitution all go through that path (`a87ce06d6`, `438bb5ba0`), more
than one bash can be live at once now that its globals are thread-local
(`09aded207`, `dc08964a8`), and it reads `/etc/bash.bashrc` like every other bash
in the guest (`63e4b5421`).

**zsh too, with completion.** `138a9c122` brings fork, per-shell state and
completion; `a78e9b80e` makes it build for the device rather than only the Mac.
It is on by default and permissively licensed, which bash is not — see
*Licensing* below.

**Job control works.** `^C` needed three separate things to stop being swallowed
(`f4c2c0b42`), `^Z` needed the dispositions a forked child would have restored
(`5a9e16a52`), and children needed their own process group (`d81abc12a`,
`df0a879b6`). `exec` in a native program is a spawn-then-wait stand-in, so every
signal aimed at the job lands on that wait — it now forwards the signal to the
program it exec'd instead of leaving it running, reparented to init
(`2ebe7af0a`), and does not mistake an interrupted wait for a dead child
(`350d3f226`).

**A task could go permanently deaf to signals, `SIGKILL` included.** A host
`SIGUSR1` wake was intermittently swallowed in a way that left the signal blocked
and pending in the target thread's own mask with the handler never running.
Measured at 93 failures in 120 trials before, 0 in 120 after. Not specific to
native programs — `sh -c 'sleep 30'` under pure emulation failed identically.
`c6b64d77d`.

**A shell can no longer kill the app by recursing.** A runaway recursive function
ran off the end of its thread's stack, which in one address space takes the whole
app down rather than one process. Both shells now refuse at the edge, including
in re-launched children. `5f5f9c88f`, `2e048c7b9`, `3a4aa6170`.

## The guest, not the host

Each of these was a native program answering a question about the wrong machine.

- **The environment** a native program sees is the guest's (`abbe0c810`), and so
  is its identity (`c21ff5a13`) and its filesystem (`759ebb8a6`).
- **Sockets and name resolution** read the guest's `/etc/hosts`, `/etc/services`
  and `/etc/resolv.conf` (`12b0e305f`, `3fe963f7b`).
- **The terminal** is described from the guest's terminfo rather than the host's
  (`c07388a6f`, `a8be23bed`), with pty and terminal control routed to match
  (`3affe3b97`, `1dd185d8e`).
- **The locale** comes from the guest's environment (`78d4a1b97`). Two device-only
  corrections followed: iOS has neither `C.UTF-8` nor `en_US.UTF-8`, only a bare
  `UTF-8` (`4b8379e29`), and that spelling is a *charset* which `LC_CTYPE` accepts
  and a whole-locale request refuses — so every native program had been falling
  back to byte semantics on a device while the guest was in UTF-8 (`cd3bf1abc`).
- **Global shell rc files** are read from wherever the rootfs keeps them.
  Devuan, Debian, Arch and Gentoo use `/etc/zsh/`; Fedora and upstream use a flat
  `/etc/`. Probed per file, so an image iSH-AOK does not ship still works
  (`59edc6a72`).
- **stdio streams are per task** (`a280a5586`), `fileno` had to be ours
  (`d7ba156d2`), and `getopt` state is per task so concurrent applets stop eating
  each other's flags (`f601a4282`).
- **A build-time gate** fails the build when a native program reaches the host
  libc (`56e2e0442`), later inverted to an allowlist (`0a1f6eef0`).

## Fork fidelity

A re-launched shell has to *become* what a forked one would have been. Rounds of
this, each found by diffing against the guest's own shell rather than against
expectation:

- Options, functions, aliases, traps, `hash -d`, readonly and hidden parameters,
  history, and `$RANDOM`'s seed and draw count all cross the boundary.
- **Globs reached the child again** — `rm -f *` had been silently deleting
  nothing (`6685fff58`).
- **MULTIOS works** (`9fabfef69`), including through every expansion that is
  itself a re-launch, and a failed target no longer starves the others
  (`253b14a1e`).
- **`bindkey` keymaps cross**, emitted as a difference from the defaults rather
  than a 400-line replay on every subshell.
- **`zstyle` patterns cross.** A style's context pattern is compiled *eagerly*, so
  replaying the `zstyle` lines inline compiled them under `zsh -f` defaults and
  `#` became a literal — every `extendedglob` style silently stopped matching in
  any subshell (`72494256c`).
- **Temporary names are unique per shell.** `mktemp`'s retry loop could never
  reach a second attempt, and `rand()` is replayed by design in a re-launched
  child, so a parent and its child were handed the same name (`0a8bbc1fd`).

Coverage for all of it ships in the guest at `/AOK/tests`: 119 zsh cases and 20
bash cases, each scored against the guest's own shell as the oracle
(`0ea71dbb3`, `68c35b52b`, `fdf8a52ea`, `59edc6a72`).

## Elsewhere

- **Root filesystems can be installed and switched from a shell** (`1cd4cc622`),
  and a failed ABI check no longer keeps a root that has none to report
  (`f6fe2d304`). A rename the guest actually sees, with a busy check that runs
  (`646a07770`).
- **`>>` on tmpfs overwrote the head of the file** — `O_APPEND` was ignored
  (`c404fb041`).
- **Two files could still be mistaken for one**, this time because finding the
  twin that swallowed an entry depended on readdir order (`42e4c038e`).
- **The crypto accelerator installer** blamed the wrong thing and hid the
  evidence (`9b7a72eed`), and its probe was a file it had mangled itself
  (`62d56e407`).
- **A bad Launch Command can no longer lock you out of the app** (`d3ee70197`).
- **Xcode stops halting on `SIGUSR2`** — the schemes now load an lldb init file
  (`f31b57d50`, `52ed7f985`).
- **Build fixes:** options added after a build directory was created are noticed
  (`68ba2ed69`); a `guest_archs` subset without riscv64 links again (`324dc25a8`);
  `iconv` lives inside libc on Linux, so demanding a standalone library broke
  every Linux CI build (`21a2a4a8c`); `<sys/sysctl.h>` is Apple/BSD-only
  (`de301eed3`).
- **`preadv`/`pwritev`** use a stack buffer for small requests, matching the
  existing fast path in `read`/`write`/`readv`/`writev` (`fa7e67c4b`, PR #554).
- **Task threads get a 4 MB stack**, with the measurement that justifies it
  recorded rather than asserted (`bc4cb15fb`).

- **`native-links.sh` can pick the shell, and cleans up after itself.**
  `--shell bash|zsh|/path`, defaulting to bash when present and zsh otherwise --
  which matters because native bash is off by default, so on a default build the
  script had been silently switching nothing (`dbaf2a2ff`). It also unlinks
  applets it linked BEFORE they were excluded: exclusions used to apply only to
  installs that had never run it, so a `dmesg` link from an older run went on
  shadowing the distro's working one for ever (`7b35aa49b`).
- **ktop truncates the command column by display width**, and never splits a
  UTF-8 character. The batch path had no limit at all (`31137f805`).
- **A long run of Linux-only build breaks was cleared.** More than a dozen
  fixes, one root cause: bash's
  `config.h` is generated by running configure on a Mac, and the tree is
  compiled for both. iconv lives inside libc on Linux; `<sys/sysctl.h>` is
  Apple/BSD-only; SmallCLUE wants BSD header visibility; `__thread` must FOLLOW
  the storage class for gcc; and `st_atimespec`, `d_namlen`, `fpurge` and its
  declaration are all Darwin spellings, and zsh's config claims <sys/filio.h>
  and <sys/sysctl.h>, which glibc does not have. Each was hidden behind the one
  before it, which is why they came out one at a time. None of this affects the
  shipped iOS app, which builds from the same tree on macOS and was green
  throughout.

## Licensing

Native bash is **GPLv3**, and linking it into the app makes that the app's
problem. It is therefore behind `-Dnative_bash`, so a build can leave the GPL out
on purpose (`723314f4f`), and the decision is written down in the README
(`41934388c`). Native zsh carries zsh's permissive licence and compiles no GPL,
which is why it is the one enabled by default (`e06c2aa5e`).

## Known gaps

Recorded rather than fixed, so nobody has to rediscover them:

- **Pattern-compilation drift.** A pattern is compiled at first use and cached
  in the parse tree with nothing recording the options in force at the time, so
  a re-launched child can compile it under different options than its parent
  did. Confirmed and reproducible; a fix needs new plumbing on `Eprog` rather
  than a change to the serialiser.
- **`pipestatus` under a multio** reports `1 0` where zsh reports `0 0`, and
  `cat <(echo A) > f | cat <(echo B)` can lose the file half -- a race real zsh
  shares.
- **A daemon can spin at 100% CPU after the device sleeps.** iOS kills connected
  sockets on sleep; reads then return ENOTCONN, which we translate to
  ECONNRESET -- correctly -- but the socket stays readable, so a poll loop gets
  "ready", then an error, for ever. Measured on chronyd at 106% of a core. The
  host is not at fault: Darwin delivers a pending socket error exactly once, as
  Linux does.
- **`fakefs_type_race` kills the CLI build on an i386 guest**, deterministically,
  in the JIT block-chaining path. Bisected with the frontend's own hatches to
  forward-edge linking -- `ISH_I386_NOCHAIN=1` is a workaround. Not seen on
  device, where the timing differs.
- **eudev does not start on Devuan.** Its init script tests for `/sys/class/`,
  which AOK's sysfs does not provide; the "requires a mounted sysfs" message is
  misleading, since sysfs IS mounted. Creating an empty `/sys/class` would make
  things worse -- the next guard sleeps 30 seconds.
- **SmallCLUE's `dmesg` says it is unsupported.** The implementation is there,
  behind `#elif defined(__linux__)`; a native program is compiled as host code,
  so it takes the `#else`. AOK already answers the syslog syscall.
- **Uptime and `btime` describe the app process, not the guest.** When iOS keeps
  the app alive for days and the guest's init restarts inside it, `wtmpdb`
  rejects the boot time as too far in the past.
