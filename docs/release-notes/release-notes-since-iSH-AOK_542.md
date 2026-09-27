# Release Notes Since `builds/iSH-AOK_542`

This is the biggest cycle in the project's history: 132 commits, dominated by one headline capability — **systemd now boots as PID 1 on the Arch Linux roots, end to end**. A stock ArchLinuxARM rootfs now comes up through systemd's real boot transaction to `multi-user.target` with a console login prompt on tty1, working DNS via systemd-resolved, real logind sessions for both console and ssh logins, `user@` managers, timers, and `systemctl` working exactly as on real Linux — with zero failed units. Getting there took roughly forty distinct kernel-emulation fixes across syscalls, sockets, procfs, tmpfs, inotify, epoll, and tty semantics, each one peeled off by booting real systemd and root-causing whatever it hit next; the fixes are individually valuable conformance work (documented below) even where the motivating consumer was systemd. The cycle also ships a host-native ChaCha20-Poly1305 crypto accelerator with an OpenSSL provider (a large ssh/scp throughput win), a wave of JIT performance work (guest-libc high-level emulation, amd64 SSE gadgets, arm64 branch fusion), several fixes for bugs that could crash the entire app from a guest, a full pre-release regression sweep run concurrently on all four guest architectures that itself surfaced and fixed six release blockers, and a batch of app UI improvements.

## Highlights

- **systemd boots as PID 1 (Arch roots, experimental).** The full arc, in rough dependency order:
  - **Getting PID 1 to start at all:** systemd ≥ 260 refuses to run without `statx` `STATX_MNT_ID` and `timerfd` `TFD_TIMER_CANCEL_ON_SET` (`2daa4a6e`); service spawning needed `clone3(CLONE_PIDFD)`, `PR_CAPBSET_DROP`, `CLONE_CLEAR_SIGHAND`, pidfd `fdinfo` "Pid:" lines, and `oom_score_adj` (`4823bd7d`, `48767a87`, `1bb03f4c`, `632e6c23`); per-service credentials setup needed the new mount API — `fsopen`/`fsconfig`/`fsmount`/`move_mount` are now implemented (`087aaed6`); mount units needed `/proc/self/mountinfo` to generate poll edges on mount-table changes (`8015a86a`); journald declared every journal corrupt until tmpfs maintained `st_nlink` (`48d8f5bc`); and sd_notify readiness needed per-datagram `SCM_CREDENTIALS` on unix datagram sockets (`605d637b`).
  - **The D-Bus boot wedge saga.** A device boot that froze at "Starting D-Bus System Message Bus" unwound into a chain of real bugs: Darwin's `accept()` ignores `SO_RCVTIMEO`, so systemd-userdbd's workers (whose whole lifecycle is driven by that timeout) blocked forever — accept is now emulated with poll and never blocks in the host (`f0c3ca19`); reopening `/proc/pid/fd` magic links ignored the caller's flags, silently handing systemd a read-only description where it asked for O_RDWR, which broke machine-id commit and put dbus-broker into a 90-second death loop (`01d36071`); and the unix-socket peer handshake sent a raw host pointer over the wire that a malicious or merely unlucky guest could get dereferenced — replaced with a validated cookie registry, fixing real host-process crashes observed under load (`8b630b9d`).
  - **Name resolution.** systemd-resolved now starts and answers queries with stock config: netlink dumps follow sd-netlink's conformance expectations (`NLM_F_MULTI` on DONE, `IFA_FLAGS`, `IFF_LOWER_UP`, real `NLMSG_ERROR` acks — `7f680207`, `855101a9`, `5d0a0c20`), multicast/PKTINFO/`IP_MTU` sockopts are handled (`aa109347`), ambient capabilities and `capset` conformance let it pass its sandbox setup (`726e9efc`), and a guest-loopback NAT lets its 127.0.0.53 stub actually carry traffic (`48e3c8bc`, `82d5218b`).
  - **The nameless-daemon mystery.** Daemons that started before dbus existed parked in sd-bus's WATCH_BIND state, inotify-watching /run for the bus socket — and iSH dropped both events they were waiting for: inotify notifications carried mount-trimmed paths that never matched watches on non-root mounts, and unix-socket `bind()` raised no `IN_CREATE` at all. Both fixed (`0c95f01e`, `362bb477`), which is what finally made logind/resolved reliably own their bus names.
  - **Login.** `/dev/console` no longer becomes a controlling terminal (the Linux rule), so getty can claim tty1 and put a real login prompt on the console (`3cb86382`, `ddfbf423`); a pidfd no longer blocks its target's exit (a systemd v255+ deadlock that stalled boots for minutes — `97bb6509`); tmpfs honors `mode=`/`uid=`/`gid=` mount options so `/run/user/1000` belongs to the user (`ac9b2fcf`); `/proc/pid/cgroup` reports real cgroup membership so `systemd --user` can allocate its manager (`ce0fe17d`); `SCM_RIGHTS` file-descriptor passing works over unix *datagram* sockets, which is how logind stores session-leader pidfds via `sd_notify` (`a2a3be33`); and dbus-broker no longer exits the moment a login creates a session scope — it calls `ioctl(SIOCOUTQ)` on any connection with pending fd-carrying messages and treats failure as fatal, so the missing ioctl killed the system bus on every single login (`fb5748fa`).
  - **The last layer: epoll-inside-epoll.** systemd's event loop watches libmount's mount-monitor epoll fd from its own epoll. iSH epoll fds were not themselves pollable and wakeups didn't cascade outward, so mount-table edges died inside the inner epoll and `tmp.mount` failed with `Result=protocol` on most boots. Epoll fds now report readiness like Linux and wakeups propagate through nested epolls (`c70d0ebd`).
  - `kcmp(2)` is implemented for all four guest architectures (systemd probes `KCMP_FILE` constantly to dedup its fd store; previously each probe logged a stub-syscall error — `f0e42484`).
- **A guest could crash the entire app by running a binary from tmpfs.** tmpfs had no `pread`, and the ELF loader called it through a NULL pointer for segments whose file content straddles EOF — host `EXC_BAD_ACCESS`, whole-process abort, found when the on-device regression suite built its test binaries under the (newly working) /tmp tmpfs mount. tmpfs now implements real `pread`/`pwrite` and the loader falls back gracefully for any filesystem without them (`56db3b8a`). Related tmpfs conformance in the same sweep: `O_TRUNC` is honored on open (`3fe3682c`).
- **Chroot path resolution had a double-prefix bug affecting every chroot.** Stored, already-normalized paths (which include the chroot prefix) were re-resolved through chroot-aware code, applying the prefix twice. Consequences inside any chroot — a first-class workflow on this fork via `/AOK/roots` — included: every `inotify_add_watch` failing ENOENT, `/proc/pid/fd` magic-link reopens failing, and util-linux `mount(8)`'s new-API path failing outright ("special device tmpfs does not exist"). Fixed with a real-root resolution mode for stored paths (`4dc7c410`).
- **iSH crypto accelerator (ChaCha20-Poly1305) + OpenSSL provider.** A new host-native crypto fast path (`84e95739` … `fbdfdd79`) exposes ChaCha20 and ChaCha20-Poly1305 AEAD to guests via a dedicated syscall, running the cipher directly over guest memory with no bounce copies, and an OpenSSL provider in `/AOK/crypto` routes guest OpenSSL (and therefore ssh/scp/sftp, which negotiate exactly this cipher) onto it. Wired into Settings with a lazy self-test (`3ad5535f`). arm64/riscv64 guests; verified against RFC 8439 vectors and differentially against OpenSSL.
- **JIT performance wave.**
  - High-level emulation of fingerprinted guest-libc functions (arm64/riscv64): recognized `memcpy`/`strlen`-family routines execute as native host code instead of instruction-by-instruction, with direct host-pointer memory ops (2–7x on the covered functions), a symbol-table attach mechanism, a copy/fill loop-idiom JIT, and a growing function set (`62895a14`, `643ea9eb`, `740e361c`, `e9277464`, `cce222b9`, `56cadc03`). Default off; enable via the environment.
  - Native amd64 SSE gadgets for the top 13 interpreter-bridge opcodes (`7532e8ea`) and arm64 `ANDS`/`TST`+`B.cond` fusion into single gadgets (`4067a8db`).
  - `fork()` no longer takes the jetsam lock once per mapped page during COW setup (`84631ecf`) — a large fork-heavy-workload win.
  - Several JIT lifecycle races fixed: a self-deadlock between teardown and invalidation locks (`d87a117d`), CLONE_VM-sibling exclusion during live munmap invalidation (`cc55759b`), an unlock-after-free in `mem_destroy` (`d9d26ef1`), and backlink scrubbing at jetsam-free time (`bca34804`). Unmapped guest-execution faults on arm64 now deliver a guest SIGSEGV (with one retry for benign races) instead of aborting the app (`75cfc2c3`, `a2e018c5`).
- **SysV message queues and semaphores are implemented** (`5562eaa3`) — unblocks `fakeroot`, and with it `makepkg` and Debian-style package builds in guests.
- **epoll membership is now keyed by (guest fd, open file description) like Linux** (`7de9386a`) — fixes spurious `EEXIST` from `epoll_ctl` after `dup()`, hit in the wild by Bun.
- **Pre-release regression sweep, all four architectures.** The full guest suite (~90 tests) was run concurrently on i386, amd64, arm64, and riscv64 CLI guests plus a three-way concurrent on-device run (native suite + chroot suite + fork storm). The sweep itself surfaced and fixed six of the bugs above (nested epoll, tmpfs exec crash, tmpfs O_TRUNC, chroot double-prefix, kcmp, and a stale test manifest); everything passes at tag time.

## User-Facing Changes

### Boot, services, and login (Arch roots)

- Booting a systemd root normally now reaches `multi-user.target` with zero failed units; console getty login, ssh logins with real logind sessions, `systemctl`/`loginctl`/`busctl`, systemd-resolved DNS, and `user@` managers all work.
- `loginctl` and friends respond instantly (a prior bus-name loss made every pam/loginctl call eat a 25-second timeout).
- The Arch provision script (`opt/AOK/tools/provision-ultimate-archlinux.sh`) had a busy cycle: it renames the image's stock `alarm` account to your chosen login instead of leaving a duplicate (`ba4ffd6e`) — now including group memberships, so Arch's periodic `shadow.service` integrity check stays green (`2c3e1785`); it restores the terminfo tree the ArchLinuxARM tarball ships stripped (fixes tmux's "missing or unsuitable terminal: xterm-256color" — `4049a6cc`); it re-enables systemd-resolved now that resolved works (`1dbcd1f7`); it repairs uid-501 ownership from older repacked tarballs (`4341a8f0`); and its motd no longer claims there is "no working init under iSH" (`b1bb9249`).
- `setup-wayland.sh` supports pacman, so the Workspace Display applet's Wayland stack (labwc/sway/foot/wayvnc/wofi) installs on Arch roots too (`85896781`).
- `uname -r` now reports `5.20.66-ish_aok` (`4ad9dff6`).

### Stability (guest-triggerable app crashes fixed)

- Executing a binary that lives on a tmpfs no longer aborts the entire app (NULL `pread` in the ELF loader — `56db3b8a`).
- The unix-socket accept handshake no longer trusts a raw pointer from the wire (host SIGBUS crashes under load — `8b630b9d`).
- `MSG_PEEK` on an fd-passing unix message no longer SIGABRTs (hit during amd64 Arch boots — `cbefe4fd`).
- The real-tty read thread hangs up cleanly on EOF/persistent error instead of spinning at 100% CPU (`cd49c54f`).
- CLI boots repair the standard `/dev` character nodes (a Docker-derived rootfs with `/dev/null` as a regular file no longer breaks everything — `51dd518d`).

### Filesystem and syscall conformance

- tmpfs: `pread`/`pwrite`, `O_TRUNC`, `mode=`/`uid=`/`gid=` root-inode mount options, and `st_nlink` maintenance (`56db3b8a`, `3fe3682c`, `ac9b2fcf`, `48d8f5bc`).
- Chroots: inotify, procfd magic-link reopens, and the new mount API all work inside a chroot now (`4dc7c410`).
- New syscalls: `kcmp` (all four guest arches — `f0e42484`); SysV `msgget`/`msgsnd`/`msgrcv`/`semget`/`semop` family (`5562eaa3`); `fsopen`/`fsconfig`/`fsmount`/`move_mount` (`087aaed6`); `preadv2`/`pwritev2`, `setdomainname`, `adjtimex`, `sched_setparam`, `sched_rr_get_interval` on arm64 (`9e96a810`, `2cc632ca`); `keyctl(KEYCTL_LINK)` no-op (`5743f4ed`); silent ENOSYS for the post-6.x xattrat family (`bd75c432`).
- Sockets: `SIOCOUTQ` (`fb5748fa`), unix-DGRAM `SCM_RIGHTS` (`a2a3be33`) and per-datagram `SCM_CREDENTIALS` (`605d637b`), `accept()` honoring `SO_RCVTIMEO` and never blocking in the host (`f0c3ca19`), `F_GETOWN_EX`/`F_SETOWN_EX` (`ef598045`), `SOCK_SEQPACKET`→`SOCK_STREAM` EPERM fallback for AF_UNIX (`957bbdac`), and the netlink conformance batch above.
- epoll: Linux-compatible membership keying (`7de9386a`) and nested-epoll readiness (`c70d0ebd`).
- inotify: full-guest-path events, `bind()` `IN_CREATE`, and parent-dir delivery of `IN_OPEN`/`IN_MODIFY`/`IN_ATTRIB` (`0c95f01e`, `362bb477`).
- `O_PATH` on sockets and FIFOs creates a path handle instead of a real open (`807ef402`); O_PATH symlink fds and `waitid(P_PIDFD)` (`007f27c6`); self-pidfd exit deadlock and an `AT_PWD` crash in O_PATH statx fixed (`3e077fb9`).
- `MAP_SHARED` writes work when protection mirroring is unavailable (`541f5bc9`).

### Performance

- ChaCha20-Poly1305 accelerator + OpenSSL provider (ssh/scp): see Highlights.
- Guest-libc HLE, amd64 SSE gadgets, arm64 branch fusion, fork COW jetsam-lock fix: see Highlights.
- `brk` reservation correctly rolls back on heap shrink (`ed396c35`) — fixes a heap-corruption class found via a `ktop` build hang.

### App UI

- The `mount -t ios` folder picker now presents from Workspace mode and even for ssh-driven guests with no foreground terminal (`217c9fad`).
- Workspace: per-window terminal font size (`7cc2c822`); Focus-on-existing-terminal no longer falsely reports "already open" (`e9220f36`).
- LLM chat: hardware Return sends, Shift+Return inserts a newline; provider picker is a pushed screen (`c319b66a`); shell tools get a configurable timeout/output cap and an auto-run warning (`cd7ad4e7`); old tool results are compacted against the model's context window (`da6fd875`).
- Session Shell signs in as root (`aa5a7260`); advanced settings are visible by default (`2cac46d0`); choosing a remote distribution actually fetches remotely (`5fe8953d`).
- `/AOK/fakefs`: a shared fakefs mount persistent across roots (`c9df4be9`).

## Known Issues

- Console getty logins do not get a logind session: logind rejects their `VTNr` because iSH has no virtual-terminal ioctls (`InvalidParameter: VTNr`). Login itself works; `loginctl` just doesn't list console sessions. ssh sessions are unaffected.
- Nested-epoll readiness covers virtual (emulated) member fds; a nested epoll whose members are all host-backed fds (real sockets/files) still needs a waiter on the inner epoll. No known real-world consumer hits this.
- Under heavy concurrent load, `ptrace_group_stop`, one `signal_child_burst` subtest, and the rusage thread-sum check can flake on-device; all pass reliably on a quiet system.
- gdb `next`/`step` immediately after a breakpoint hit on arm64 can still misbehave (issue #503, carried forward).
- riscv64 `sudo` "no gadget 00000000" (PC jump to zero) remains unreproduced (carried forward).
- systemd as PID 1 is supported on the Arch roots and considered experimental; Alpine (OpenRC) and Devuan (sysvinit) roots continue to use their own inits.

## Maintainer Notes

- `fs/aok-tests.manifest` is an explicit list; a new regression test must be added both to `tests/manual/setup-regressions.sh` and to the manifest or the bundled `/AOK/tests` copy silently lacks it (`01c33b2f` fixed six such omissions).
- The regression suite grew substantially this cycle: `epoll_nested`, `tmpfs_exec`, `kcmp`, `siocoutq`, `inotify_mount_paths`, `scm_rights_pidfd`, `accept_rcvtimeo`, `procfd_reopen`, `statx_mnt_id_timerfd`, `hle_loop`, `ands_bcond_fusion`, crypto vectors, and more — ~90 tests total, all validated against real Linux (the mint oracle) as well as all four guest architectures.
- Release-testing procedure now includes the concurrent multi-root device run (suites chrooted into `/AOK/roots/*` plus a fork storm, all sharing one emulator process); it caught two of this cycle's blockers that no single-suite run had ever hit. `mount-root.sh <name> -- CMD` word-splits its command; pass a script file.
- The downloadable-rootfs list moved to the `deps/rootfs-manifest` submodule with archives committed directly (`8b36ab79`, `aad9c472`, `d69cd118`); Arch tarballs were republished with uid and pam_nologin fixes (`c6f3331f`, `fa2d5a2f`).
- Docs were reorganized under `docs/` (`60b9b828`, `0773723a`, `de48789d`).

## Commit Range

```
0673cc2b docs: revise the 2026-07 performance-optimizations write-up
217c9fad app: present the ios-mount folder picker without a foreground terminal
2c3e1785 provision-arch: fix group memberships when renaming the stock 'alarm' user
3fe3682c tmpfs: honor O_TRUNC on open
4dc7c410 fs: stop double-applying the chroot prefix to stored normalized paths
f0e42484 kernel: implement kcmp(2) (systemd fd-store dedup; stub-272 log spam)
56db3b8a tmpfs: implement pread/pwrite (execve from tmpfs crashed the whole app)
c70d0ebd kernel/epoll: epoll-inside-epoll readiness (systemd tmp.mount 'protocol' failure)
01c33b2f aokfs: ship the six missing regression sources in /AOK/tests
f180e7a0 fs/sock: drop leftover scm-recv debug printk
fb5748fa fs/sock: implement SIOCOUTQ (dbus-broker died on every login's session scope)
85896781 setup-wayland: add pacman (Arch) support
b1bb9249 provision-arch: motd/services text no longer claims "no working init under iSH"
4049a6cc provision-arch: restore stripped terminfo tree (tmux "missing or unsuitable terminal")
a2a3be33 fs/sock: SCM_RIGHTS over unix DGRAM sockets (sd_notify FDSTORE)
ce0fe17d cgroup2: track membership from cgroup.procs writes, report it in /proc
ac9b2fcf tmpfs: honor mode=/uid=/gid= mount options on the root inode
97bb6509 kernel: a pidfd must not block its target's exit
3cb86382 tty: /dev/console never becomes the controlling terminal (Linux rule)
92daec98 tests: inotify_mount_paths -- events on non-root mounts + bind() IN_CREATE
362bb477 kernel/inotify: deliver IN_OPEN/IN_MODIFY/IN_ATTRIB to parent-dir watches
0c95f01e fs: fire inotify with full guest paths, and on unix socket bind
8c668f89 tests: extend netlink_route with sd-netlink dump conformance checks
1dbcd1f7 provision-arch: re-enable systemd-resolved -- it works under iSH now
aa109347 fs/sock: IP/IPv6 multicast sockopts, IPV6_RECVPKTINFO, IP_MTU
855101a9 fs/sock: report IFF_LOWER_UP alongside IFF_RUNNING in netlink link flags
7f680207 fs/sock: netlink dump conformance -- NLM_F_MULTI on DONE, IFA_FLAGS in addr dumps
ba4ffd6e provision-arch: rename ArchLinuxARM's stock 'alarm' account instead of duplicating it
1905f146 provision-arch: disable systemd-resolved -- it blocks ALL name resolution
ddfbf423 dev: provide /dev/tty0 so systemd's getty@tty1 puts a login on the console
8b630b9d fs/sock: replace the raw-pointer unix peer token with a validated registry
d1cd02a8 fs/sock: ISH_FORCE_SEQPACKET_EPERM test hook for sandbox-parity debugging
01d36071 fs/generic: honor the caller's flags when reopening /proc/pid/fd magic links
fa2d5a2f deps: bump rootfs-manifest to the pam_nologin-patched Arch tarballs
f0c3ca19 fs/sock: never block in the host accept(); honor SO_RCVTIMEO like Linux
541f5bc9 fs/mmap: pre-grant host write for MAP_SHARED when protection mirroring is unavailable
da6fd875 llmchat: compact old tool results and size it against the model's context window
8e234f4a llmchat: raise shell-tool timeout cap to 15 minutes
c9df4be9 fs: add /AOK/fakefs, a shared fakefs mount persistent across roots
5562eaa3 kernel: implement SysV message queues and semaphores (fakeroot/makepkg)
7de9386a fs/poll: key epoll membership by (guest fd, file description) like Linux
b0fe226d Testing Discord SYnc
5fe8953d app: make choosing a remote distribution actually fetch remotely
703c9cb9 opt/AOK: remove the pam_nologin bypass
84631ecf memory: stop fork's COW setup from taking jetsam_lock per mapped page
3b425164 app: defer TerminalView keyboardAppearance's first-responder dance
0c264169 kernel: silence add_key/bpf stub logging -- benign, gracefully-handled ENOSYS
d87a117d jit: fix self-deadlock between jit_teardown_lock and jit_invalidate_lock
cc55759b jit, emu/memory: exclude CLONE_VM siblings during live munmap invalidation
e9220f36 workspace: fix Focus-on-existing-terminal falsely reporting "already open"
957bbdac sock: fall back to SOCK_STREAM on EPERM for AF_UNIX SOCK_SEQPACKET too
cd7ad4e7 app: LLM chat shell tools -- configurable timeout/output cap, auto-run warning
a2e018c5 jit: retry unmapped guest-execution faults on arm64 before delivering SIGSEGV
af249177 provision-arch: disable pam_nologin -- terminals must work during a stalled boot
75cfc2c3 jit: deliver guest SIGSEGV for unmapped guest-execution faults, not abort
d9d26ef1 jit, emu/memory: fix unlock-after-free of the teardown lock in mem_destroy
bca34804 jit: scrub surviving backlinks at jetsam-free time; bound the jumps_from walk
48e3c8bc fs/sock: guest-loopback NAT -- systemd-resolved starts for real
82d5218b fs/sock, kernel/inotify: four fixes on systemd-resolved's startup path
2092ead9 fs/sock: real multicast netlink notification delivery (infrastructure, not yet enabled)
5d0a0c20 fs/sock: real NLMSG_ERROR ack for non-dump rtnetlink requests
6a1d4d51 fs/sock: accept SO_REUSEADDR/SO_REUSEPORT on fake netlink sockets -- udevd boot wedge
3fb8ffd0 jit: exclude live sibling threads from process-exit JIT teardown
2cac46d0 app: show advanced settings by default, drop long-press reveal
be455b5c fs/sock, main: fix amd64 systemd boot wedge -- spurious MSG_TRUNC on notify + missing /dev/console
cbefe4fd fs/sock: don't crash on MSG_PEEK of an fd-passing message (amd64 Arch boot SIGABRT)
c6f3331f deps: bump rootfs-manifest to republished uid-fixed Arch tarballs
4ad9dff6 kernel: advertise kernel release 5.20.66-ish_aok
9ad69616 kernel: fix stale uname version in xattrat stub comment (5.20, not 4.20)
bd75c432 kernel: syscalls 463-469 (xattrat family, open_tree_attr, file_[gs]etattr) get silent ENOSYS stubs
4341a8f0 provision-archlinux: repair uid-501 ownership from the old repacked tarballs
726e9efc kernel: ambient capabilities, SECBIT_KEEP_CAPS, capset/capget conformance -- resolved passes step USER
807ef402 fs/generic: O_PATH on sockets and FIFOs creates a path handle, not a real open
605d637b fs/sock: per-datagram SCM_CREDENTIALS on unix dgram sockets -- systemd boots to multi-user.target
48d8f5bc fs/tmp: maintain st_nlink -- journald no longer declares every journal file corrupt
8015a86a fs/proc, fs/mount: generate mountinfo poll edges on mount-table changes -- mount units finally work
5743f4ed kernel: keyctl -- accept KEYCTL_LINK as a no-op
087aaed6 fs/mount, kernel: implement fsopen/fsconfig/fsmount/move_mount -- unblocks systemd's per-service credentials setup
1bb03f4c fs/proc, kernel/pidfd: add "Pid:" fdinfo line for pidfds -- unblocks every clone3 service spawn
48767a87 kernel: clone3 -- accept CLONE_CLEAR_SIGHAND instead of ENOSYS
632e6c23 kernel, fs/proc: implement oom_score_adj, fix proc .update error propagation
4823bd7d kernel: clone3(CLONE_PIDFD) and PR_CAPBSET_DROP -- unblock service spawning
eeca13ad docs: HLE enhancement wave in the July performance doc
3e077fb9 kernel, fs: fix self-pidfd exit deadlock and AT_PWD crash in O_PATH statx
7cbaedb3 tests: hle_loop -- arm64 copy/fill loop idiom regression test
56cadc03 jit/hle: symbol-table attach, call statistics, and copy-loop idiom JIT
2b11bc4b jit/hle: riscv64 RVC function entries were excluded by the 4-alignment gate
51dd518d main: repair the standard /dev char nodes at every CLI boot
cd49c54f fs: real tty read thread hangs up on EOF/persistent error instead of spinning
007f27c6 fs, kernel: O_PATH symlink fds + waitid(P_PIDFD) -- next systemd-as-PID-1 layer
85c32e81 tests: ship statx_mnt_id_timerfd.c in /AOK/tests (missed manifest entry)
2daa4a6e kernel: unblock systemd >= 260 as PID 1 (statx STATX_MNT_ID + timerfd TFD_TIMER_CANCEL_ON_SET)
fbdfdd79 crypto: add ChaCha20-Poly1305 AEAD to the iSH OpenSSL provider
3ad5535f app: wire the crypto accelerator into Settings; lazy self-test
de48789d docs: fix README links after moving docs into docs/
0773723a docs: move design/plan/technical docs into docs/
60b9b828 docs: move release notes into docs/ to declutter the repo root
ced83a2d docs: performance optimizations (July 2026) with benchmarks
6a163c2f opt/AOK/crypto: OpenSSL provider routing ChaCha20 to the accelerator
dfd42c7a kernel/ish_accel: raw ChaCha20 stream op (alg=1) -- the cipher ssh calls
5a21306f kernel/ish_accel: run the cipher directly over guest memory (no bounce copy)
3a6f50cd kernel/ish_accel_crypto: streaming AEAD API; one-shot now wraps it
38f9239d kernel/ish_accel: reuse a per-thread scratch buffer (no malloc on hot path)
a5cf0e75 tests: iSH crypto accelerator guest test (RFC 8439 + OpenSSL differential + throughput)
84e95739 kernel: iSH crypto accelerator -- host-native ChaCha20-Poly1305 (Phase 1)
cce222b9 jit/hle: add strncat/stpncpy/rawmemchr and the strspn/strcspn/strpbrk trio
e9277464 jit/hle: add strcpy/stpcpy/strncpy/strcat/strrchr/strnlen/memrchr
27ec7d48 tests: add chacha20-poly1305 RFC 8439 vector + throughput harness
740e361c jit/hle: add strcmp/strncmp/memchr/strchr to the HLE function set
643ea9eb jit/hle: direct host-pointer memory ops (fixes ~2x pessimization -> 2-7x win)
ef598045 fs: handle fcntl F_GETOWN_EX/F_SETOWN_EX (fixes F_GETOWN)
8f83f6ab jit/hle: near-miss candidate tracer; tests: ship ands_bcond_fusion in /AOK
d69cd118 Bump deps/rootfs-manifest: archives now committed directly in the repo
aad9c472 Bump deps/rootfs-manifest: host archives in the submodule's own release
8b36ab79 Move downloadable rootfs list to deps/rootfs-manifest submodule
62895a14 jit: HLE of fingerprinted guest libc functions (arm64/riscv64, default OFF)
7532e8ea jit: native amd64 SSE gadgets for the top 13 interpreter-bridge opcodes
4067a8db jit: fuse ANDS/TST+B.cond into single gadgets (#504)
e1b56bf9 fs: fix systemd boot freeze on Arch Linux ARM (mount-point statx + devtmpfs)
ba42e699 kernel/calls: fix duplicate case 121 in handle_asm_generic_native_syscall
e70c32a9 tests: add arm64 ANDS+B.cond fusion regression coverage (#504)
2cc632ca kernel: fix preadv/pwritev offset, implement setdomainname, adjtimex, sched_setparam, sched_rr_get_interval for arm64 (#496)
033a094b kernel: remove stale ENAMETOOLONG todo (#495)
9e96a810 kernel: wire preadv2/pwritev2 for arm64 guests (syscalls 286/287) (#494)
5d1cd26c jit/emu: keep the arm64 debug tools from the brk-reservation investigation
ed396c35 kernel/mmap: roll back brk_reserve_start on heap shrink
aa5a7260 app: Session Shell always signs in as root
7cc2c822 app: per-window terminal font size in Workspace mode
3f6d9a3d 🎨 Palette: [UX improvement] (#497)
34cf9af9 docs: merge 541 and 542 release notes into one since-540 cycle
c319b66a app: hardware Return sends the LLM prompt, Shift+Return newlines; provider picker as a pushed screen
1b680324 build: sync the project-level default CURRENT_PROJECT_VERSION to 542
```
