# Release Notes Since `builds/iSH-AOK_548`

177 commits, and one of them is the release: iSH-AOK can now run programs that
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

That fix mapped only the type-0 case, and the rest of the class was written down
as "now unreachable via mknod". It was not. Any *other* invalid `S_IFMT` --
`0x3000`, say -- walked through `generic_mknodat`, which rejects only DIR and
LNK and gates only BLK and CHR on superuser, and produced the same
inode-of-no-type; `read`, `pread`, `pwrite` and `ftruncate` each still aborted
the app, from three lines of C on any mounted tmpfs, with no privilege required.
`kernel/fs.c` now whitelists the five types Linux's `may_mknod` accepts and
answers EINVAL otherwise, so the inode cannot be created at all, and the five
reachable asserts became errno returns anyway (`a237b521f`).

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
(`0c7de3507`, `c36592960`). `/AOK/tools/native-links.sh` also switches the UID 1000
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
  `--shell bash|zsh|/path`, defaulting to bash when present and zsh otherwise.
  Before it there was no way to ASK for the native zsh at all -- the target was
  hardcoded -- and on a build configured with `-Dnative_bash=disabled` the file
  does not exist, so the script had been silently switching nothing (`dbaf2a2ff`).
  It also unlinks
  applets it linked BEFORE they were excluded: exclusions used to apply only to
  installs that had never run it, so a `dmesg` link from an older run went on
  shadowing the distro's working one for ever (`7b35aa49b`).
- **ktop truncates the command column by display width**, and never splits a
  UTF-8 character. The batch path had no limit at all (`31137f805`).
- **Linux CI builds again, on both compilers.** It had been red since
  2026-08-10 -- before 548 shipped, so the commit tagged 548 was itself red.
  Nearly all of it was one root cause: bash's, zsh's and OpenSSH's `config.h`
  are each generated by running configure on a Mac, and the tree is compiled for
  both platforms, so all three assert Darwin facts that are false on glibc.
  iconv lives inside libc on Linux; `<sys/sysctl.h>` and `<sys/filio.h>` are not
  glibc headers; `st_atimespec`, `d_namlen` and `fpurge` are Darwin spellings;
  `strtonum`, `timingsafe_bcmp`, `memset_s`, `<util.h>`, the `pw_class` family
  and `sin_len` are BSD's. Every such macro is now behind `!__linux__`, so the
  shipping build is bit-for-bit unmoved. The rest: `__thread` must FOLLOW the
  storage class for gcc (40 declarations); `-D_GNU_SOURCE` project-wide; a real
  xattr port, since Darwin's calls carry a position and an options word and
  Linux's do not; a stand-in for `<rpc/types.h>`, which OpenSSH asks for and
  Debian hides in libtirpc, and which alone accounted for 181 of the original
  186 failures; the fused i386 ALU gadgets, which exist only in aarch64 assembly,
  so merely naming them was a link error on any x86_64 host; and a duplicate
  `smallclueRunRsync` that ld64 tolerates and GNU ld does not.

  **One of those was not a build fix at all.** GCC rejected an assignment clang
  waves through, and behind it was a genuine crash: `bash --rcfile FILE` and
  `--init-file FILE` wrote through a NULL pointer. `long_args[]` has two pointer
  members, and the generator that moved its addresses into a runtime fixup wrote
  every entry to the integer one -- leaving `char_value` NULL for the only two
  entries bash dereferences it for. Native bash runs in-process, so that is the
  app going down rather than a shell. It was introduced and fixed inside this
  cycle, so no released build ever carried it, but it is the argument for
  keeping a second compiler in CI.

## Validation

**CI is green on both compilers** — `build-mac`, `build-linux (clang)` and
`build-linux (gcc)`, plus `Build Dev IPA`. That is the first fully green run
since 2026-08-10, which predates the 548 release: the commit tagged 548 was
itself red.

The Linux side was additionally checked the way CI does it rather than in place:
a fresh `git clone --recurse-submodules` of this branch on a Debian 13 host,
built with each compiler — 0 failed targets both ways — then `meson test`
`float80` and `riscv64_decode` (pass, both), and the `e2e` suite (pass, 212s).

**On device**, a 4-architecture run: the booted aarch64 arm was 118/118 clean;
the three chroot arms were i386 110/6, x86_64 112/5, riscv64 103/5. Five of
those failures are identical on every chroot arm and absent from the booted one
— `devtmpfs_mount`, `proc_pid_io`, `taskstats_genl`, `mount_stdev`,
`fifo_open_creat_deadlock`. `mount_stdev` and `devtmpfs_mount` are proven chroot
artifacts, since they also fail in an *aarch64* chroot, the arch that passes
them when booted: iSH-AOK has no mount namespaces, so `/proc` inside a chroot
describes the booted root while `stat()` sees the chroot's. The other three pass
when run by hand in the same chroot. i386's sixth failure is not individually
recorded, and is the one gap in this account.

**On the CLI**, after the `mknod` fix below, the aarch64 guest suite is 114
pass / 6 skip / 1 fail, that one being `ptrace_group_stop` timing out under a
heavily loaded host — it passes 3 for 3 when run alone, and makes no filesystem
calls at all. The `mknod` change itself was checked against every `S_IFMT`
value: the six invalid ones now answer EINVAL, `S_IFREG`, `S_IFIFO`, `S_IFSOCK`,
`S_IFCHR` and a bare mode still succeed, and DIR and LNK are unchanged.

Native zsh's differential suite (`/AOK/tests/native_zsh_fork_state.sh`) is
116 of 119, the three failures being process substitution — see *Known gaps*.

## Licensing

Native bash is **GPLv3**, and linking it into the app makes that the app's
problem. It is therefore behind `-Dnative_bash`, so a build can leave the GPL out
on purpose (`723314f4f`), and the decision is written down in the README
(`41934388c`). That option defaults to `auto`, which resolves to ON whenever
`deps/bash` is checked out -- so the shipped build DOES carry native bash, and
leaving the GPL out is a deliberate `-Dnative_bash=disabled`, not the default.
Native zsh carries zsh's permissive licence and compiles no GPL (`e06c2aa5e`);
it is enabled too.

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

## Commit Range

`builds/iSH-AOK_548..builds/iSH-AOK_549`

```
d101eeb07 docs: two pages on native programs, and the gaps around them
4dba625a4 docs: twenty stale claims across the README, /AOK/docs and the tools
1c0c5ddf7 docs: seven corrections that would have misled a 549 user
39a3c289c docs: two TODO entries -- one closed, one opened with a stack
d7d820af1 ci: the release workflow has been publishing placeholder bodies since 544
61b6d03da docs: native bash is ON by default -- correct the claim and its three copies
a237b521f fs: an invalid mknod type could abort the whole app
380accb4e docs: Linux CI is green, and btop's blank panels have a cause
f078b4f36 build: bump deps/bash, deps/smallclue and deps/zsh
3d1b76645 tools: map the long_args slot to the member it fills
65a6bde0d build: the plumbing the Linux build needs
0bca896e2 build: port the native libc shim to glibc
96525d439 build: fix two glue declarations GCC and GNU ld reject
53efeba22 jit: fall back to the unfused ALU expansion off aarch64
00a93dae2 docs: a TODO list for what is open after 549
67a7407cf build: give zsh _GNU_SOURCE, and bump deps/zsh for the Linux header fixes
fc1c1b3d8 release: bump the build number to 549
be122507a build: bump deps/zsh for the glibc header fixes, and stop the notes overclaiming
c1c135eb4 docs: bring the 549 notes up to the whole release
fe294a6c4 build: the bash-shim header needs the __thread reorder too, and fpurge a declaration
31137f805 ktop: truncate the command by display width, and never split a UTF-8 character
862f2c381 build: bump deps/bash for the d_namlen and fpurge platform fixes
7e082e396 build: bump deps/bash for the stat-time platform fix
11edc1843 fs: mknod with no type bits makes a regular file, and tmpfs must not abort
55d53227b fs: an anon_inode fd is a regular file, and its name has one set of brackets
452406259 proc: the v6 socket tables need the v6 header, which is how readers detect them
7b35aa49b tools: native-links.sh removes links it made before an applet was excluded
dbaf2a2ff tools: native-links.sh can switch the login shell to zsh, not only bash
e0624a620 build: bump deps/bash for the doubled __thread fix
f15ed19dd build: let <sys/file.h> land before the flock macro, and give bash _GNU_SOURCE
345e906ff tools: emit `extern __thread`, not `__thread extern`, and bump deps/bash
63ad0e831 native: answer strchrnul ourselves, or older devices call address zero
28ae39133 build: SmallCLUE expects BSD header visibility, so give it _GNU_SOURCE on Linux
caeb436a3 docs: add iSH-AOK 549 release notes
fa7e67c4b ⚡ Bolt: use a stack buffer for small preadv/pwritev (#554)
de301eed3 build: <sys/sysctl.h> is Apple/BSD-only, so do not include it elsewhere
21a2a4a8c build: iconv lives in libc on Linux, so stop demanding a library that is not there
59edc6a72 tests: global rc files, and bump deps/zsh for the probed etcdir
72494256c tests: zstyle patterns cross a re-launch, and bump deps/zsh for it
fdf8a52ea tests: user math functions cross a re-launch, and the reported defect does not
0a8bbc1fd native: the temporary-name retry loop could never retry
cd3bf1abc native: Darwin's "UTF-8" is a charset, so give it to LC_CTYPE and not to LC_ALL
68c35b52b tests: cover round three, and bump deps/zsh for it
2ebe7af0a native: a signal aimed at a native job must reach the program it exec'd
42e4c038e fakefs: finding the twin that swallowed an entry must not depend on readdir order
4b8379e29 native: iOS spells the UTF-8 locale "UTF-8", and has no other
af4387b85 tests: cover bash's recursion guard and the guest-locale fix
253b14a1e multio: a failed target must not starve the others
bc4cb15fb task: record what the 4 MB stack was measured to buy
78d4a1b97 native: a program's locale comes from the guest's environment
5f5f9c88f native: a shell must not recurse off the end of its thread stack
3a4aa6170 tests: the recursion app-killer is closed, including in re-launched children
2e048c7b9 native: WIP snapshot of the recursion stack guard
324dc25a8 jit: a guest_archs subset without riscv64 links again
c07388a6f native: terminal capabilities come from the guest, not the host
3f62c273c vi: cw at the end of a line no longer joins the next one
388136408 native: round two of the fork-fidelity work
f31b57d50 debug: schemes load the lldb init file, so Xcode stops halting on SIGUSR2
6685fff58 native: globs reach a re-launched child again
0181c8aef tests: cover MULTIOS, now that it works
9fabfef69 native: MULTIOS in zsh, via the zsh-multio byte pump
52ed7f985 debug: silence the second wake signal, and give lldb a config at all
89ee2554a native: snapshot of round-two work in progress
0ea71dbb3 tests: durable coverage for the native shells' fork state
c6b64d77d kernel: a task must not go deaf when a host wake is swallowed
e25b62e89 native: a shim stream now tells stdio which way it runs
350d3f226 native: a signal is not an answer to "did my child finish"
3282f865f native: bash keeps command-substitution exit status again
138a9c122 native: zsh gets fork, per-shell state, completion, and honest host routing
a78e9b80e native: zsh builds for the device, not just the Mac
68ba2ed69 build: notice options added after a build dir was created
e06c2aa5e native: zsh as a third native program, on by default and honest about its limits
958bd80a5 native: id can look up users
942527b95 native: uptime reports users and load average, like procps
0aa64ec78 native: uptime reported zero — a per-task conversion that should not have been
41934388c docs: put the native bash licensing decision in the README
723314f4f build: -Dnative_bash, so a build can leave the GPL out on purpose
a665268d7 native: the ssh family says its own name, and the gate stops exempting a namespace
f601a4282 native: getopt state per task, so concurrent applets stop eating each other's flags
e47ee15d2 native: pick up the find argv fix
5e46eedf1 native: route what OpenSSH was reaching past the shim for
7eae8fb7c native: pick up the applet argv fix
7e58b2e4d native: native_libc.c includes the headers whose constants it uses
a8be23bed native: describe the terminal properly, not just enough to drive one
8d57bc512 native: ssh-copy-id reports what it did
e5924f326 tools: the availability probe must not generate an SSH key
e3dc52826 native: ssh prompts for a password again, and the gate can see openssh
a280a5586 native: stdio streams are per task, so a pipe between two applets works
ec07efdbe native: smallclue grep learns -q
6e348924a native: the iOS app builds with ssh in it
3fe963f7b native: name resolution, and the last of the broken applets
5e008fa61 fs: a trailing "." or ".." is EEXIST, not a permission failure
81686796e native: openssh is a real fork now, not a snapshot
1d9e3a5fd native: point at smallclue carrying openssh as a submodule
b732cadea native: ssh, scp, sftp and ssh-keygen are real programs now
c36592960 tools: unstick native-links.sh's exclusion list, and put the links on PATH
34f066e2d tools: native-links.sh switches the UID 1000 login shell too
a87ce06d6 native: subshells reach parity — the state script's per-line redirections
438bb5ba0 native: subshells run our own bash now, and the TLS tools cover smallclue
dc08964a8 native: lift the one-live-bash restriction
09aded207 tools: make bash's globals thread-local, so more than one can be live
646a07770 roots: a rename that the guest sees, and a busy check that runs
05ba9d1ad deps: bash that keeps its line editing across invocations
55e0e4e7c native: say in the log when a bash hands over or comes straight back
63e4b5421 build: native bash reads /etc/bash.bashrc, like every other bash in the guest
5f8276a47 docs: the state script's ordering rules
9d031e839 docs: two more divergences the re-launch cannot close
fd68cc0b5 docs: the state script's ordering rules, and what the re-entry audit missed
ef91a5f26 deps: bash that closes its children's spare pipe ends, and reads its profile every time
4982814dc native: exec must not hand on the shim's blocking either
803271bc7 native: the guard that latched, and the trigger it turned off
16fdd82ee docs: what native bash does now, measured against the emulated one
ed6828008 deps: bash with the re-entry audit implemented
ed5bcc6a8 native: clear shell_name before re-entering bash
6cb9a6a2e deps: bash whose children can be stopped
5a9e16a52 native: ^Z, which needs the dispositions a forked child restores
d81abc12a deps: bash that puts its children in their own process group
f4c2c0b42 native: ^C, and the three ways the shim was swallowing it
3dd40abc5 docs: the full re-entry list, because the abbreviated one was dangerous
71e796a12 native: a task must not inherit the pointers its parent owns
a355098ab docs: a failed spawn currently looks like success
caaa8fe79 docs: the re-entry problem, and the twenty globals it turns into crashes
2b38c9a20 deps: bash that waits for its children
8e95579fc native: setlocale, the service database, and bash with readline
d7ba156d2 native: fileno must be ours, which is why an interactive bash exited
f4d981ce2 deps: bash with fork fully replaced -- subshells and pipelines work
ff7b7549e build: say so when deps/bash is stale, instead of naming a missing file
0a1ee4e2a native: posix_spawn, and bash's login shell comes up
c8006d0c2 docs: what fork actually costs, now that bash is running
c682d4251 native: bash, plugged in and running
738b0b02c native: route the rest of the kernel surface, not just what bash asked for
ea8c14368 native: generate the syscall numbers, so all of the kernel is reachable
6d5671a6d deps: bash 5.2.21 as a submodule, and it compiles under the shim
37e9f6265 native: two seam gaps that compiling bash exposed, and one it did not
3bc2f966b docs: measure the bash question, and reverse the answer on fork
d3ee70197 app: a bad Launch Command must not lock you out of the app
1ab8842a7 docs: what native bash needs, measured rather than guessed
2af64f723 deps: SmallCLUE with Nextvi's spawn seam, so `!!` works in vi
5ffb65a55 tools: bring the two native-code gates back in step with reality
158127f6c native: vi works more than once, and can run a command
d47ff53ac native: popen and pclose
df0a879b6 native: a spawned child gets its process group and its stdio
478398189 build: compile the real Nextvi, so `vi` is an editor
1dd185d8e native: pipes and the terminal's foreground group
e49c62df3 native: restore the tar/gzip stubs so working links again
12b0e305f native: sockets, and name lookup from the guest's files
3affe3b97 native: pty, terminal control, and the rest of the host-answering odds
3d1d12741 native: signals and session control for a native program
abbe0c810 native: the environment a native program sees is the guest's
fbcd3ed59 native: the libc shim calls syscalls, not the VFS
acb90c0ab native: a native program can issue guest syscalls
0a1f6eef0 tools: invert the native-libc guard to an allowlist
c21ff5a13 native: report the guest's identity, not the device's
abe56172f native: a native program can start a guest process
110b3cbf6 native: survive threads a native program creates itself
9c9bce7e9 deps: bump smallclue for top's 'q'
575592ff6 native: generalize the libc shim, and bridge termios
6b7a40b04 native: make native programs interruptible
0c7de3507 tools: script to link SmallCLUE's applets, into a directory off PATH
f780b9989 native: route the shim through AOK's kernel, and fix a stack smash in realpath
11b0b507c native: fix the iOS build the SmallCLUE integration broke
f8152644f native: compile the real SmallCLUE into iSH-AOK
622ab23f8 native: support the busybox-style `smallclue <applet>` form
9eeaf5fef deps: bump smallclue to the spawn-without-fork helper
56e2e0442 tools: fail the build when native programs reach the host libc
bc15a59f7 deps: vendor smallclue as a submodule
759ebb8a6 native: give native programs access to the guest filesystem
1a01a3a7d native: run the program after execve frees its buffers, not instead of returning
3f9923a0d native: run programs compiled into iSH-AOK instead of translating them
f6fe2d304 roots: an imported archive has no ABI to report, and a failed check kept the root
1cd4cc622 roots: install and switch root filesystems from a shell
62d56e407 crypto: the installer's probe was a file it had mangled itself
c404fb041 fs: tmpfs ignored O_APPEND, so every >> overwrote the head of the file
9b7a72eed crypto: the accelerator installer blamed the wrong thing and hid the evidence
34f228776 docs: bring the Korean and Chinese READMEs up with the English one
c745dd5df docs: the README described a fork that no longer exists
```
