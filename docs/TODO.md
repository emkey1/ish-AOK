# iSH-AOK TODO

Open work: bugs that are diagnosed but not fixed, reported issues, and features
deferred on purpose. Each entry says what is already **established**, so nobody
re-derives it, and what the **next step** actually is.

Started 2026-08-19, after the 549 release run.

---

## Diagnosed, not fixed

### Terminal cell height vs Powerline/block glyphs

At font sizes under 16 a background highlight sits 1-2px proud of the Powerline
separator next to it. Reported as "add a setting for cell height or baseline
offset".

**Established** (from two on-device screenshots, 2026-08-19):

- Cell **backgrounds** tile perfectly: a run of background-coloured spaces is a
  solid column with no seam between rows.
- Block **glyphs** do not. `█` leaves a uniform dark band between every row, and
  on a red background the red shows through at the **top** of each cell.
- The band is the same on every row, so this is NOT sub-pixel rounding or
  per-row drift. It is a constant mismatch.
- Cause: hterm sizes a cell as `fontBoundingBoxAscent + fontBoundingBoxDescent`
  (hterm_scrollport.js, `measureCharacterSizeModern`) -- the font's MAXIMUM
  extent, including room for accents and deep descenders that `█` does not use.
  The background fills that cell; the glyph only rises to about the em/cap
  height. The leftover ascent is the band.
- Why it depends on size: the band is a fixed fraction of the em, so it shrinks
  with the font but cannot go below one whole pixel -- under ~16pt it stops
  being invisible.

**Next step.** Implement the reporter's FIRST suggestion, cell height, not the
second. A baseline offset cannot fix a glyph that is shorter than its cell; it
just moves the band from the top to the bottom. Expose a **line-height
multiplier** rather than a hardcoded formula, because how much of the em a
patched font's blocks cover varies between Nerd Font patches. Default it to
today's behaviour so nobody's setup shifts.

Caveats: this is in `deps/libapps`, our vendored hterm fork, so it is a
submodule change; and shrinking cell height risks clipping tall glyphs, so it
needs visual checks across sizes plus an accented and a CJK sample.

### chronyd (or any poll loop) spins at 100% CPU after the device sleeps

**Established.** Measured on ip5-dev at 106% of one core, state S. 12s of
strace: 23748 `pselect6`, 47496 `clock_gettime`, 47496 `recvmmsg` -- every
`recvmmsg` failing with ECONNRESET on the same two fds, which `pselect` keeps
reporting readable.

The cause is in `fs/sock.c`'s `sock_translate_err`, whose own comment says it:
iOS kills connected sockets when the device sleeps, reads then return ENOTCONN,
and we translate that to ECONNRESET. The socket is dead for good, so the error
is re-delivered for ever while `sock_poll` goes on saying "readable". Poll says
ready, recv says error, nothing changes.

The host is NOT at fault, measured rather than assumed: a native macOS probe
(`scratchpad/udperr2.c` pattern) shows Darwin delivering a pending socket error
exactly once and then reporting the socket unreadable, which is Linux's
behaviour too.

**Next step.** Once that translation fires the socket is finished, so record it
on the fd and have `sock_poll` return `POLL_ERR|POLL_HUP` instead of
`POLL_READ` thereafter -- what Linux shows for a reset connection, and what
makes a poll loop close or reconnect rather than spin.

**Caveat: not verifiable on demand.** The state needs a socket killed by a
device sleep. Both devices read 0% now. Do not read 0% as evidence of a fix.

Secondary: the host and Linux both report ECONNREFUSED for a connected UDP
socket on ICMP port-unreachable; the guest sees ECONNRESET. Check the mapping.

### i386: `fakefs_type_race` kills the CLI build

**Established.** On an i386 guest the CLI build dies deterministically (3/3, no
output); x86_64 passes 3/3.

    thread #1058, name = 'fakefs_-1250', EXC_BAD_ACCESS (code=1, address=0x1c)
    frame #0: cpu_step_to_interrupt(...) at jit.c:1774

That line dereferences `last_block->jump_ip[0]`, and the comment above it warns
about exactly this: pointers to a block already marked jetsam.

Bisected with the frontend's own hatches:

    (no hatch)             rc=139   crash
    ISH_I386_NOBACKCHAIN=1 rc=139   crash    <- forward edges only
    ISH_I386_NOCHAIN=1     PASS              <- no linking at all

So it is forward-edge block chaining, NOT the newer backward-edge work.
Workaround: `ISH_I386_NOCHAIN=1`.

Ruled out by measurement: emulated CPU count (crashes at 1, 2 and 4 alike), and
the 2026-08-19 mknod change (reverting just that hunk leaves it identical).

The DEVICE build does not crash -- the i386 arm ran this test during the 4-arch
run with the app staying up -- so treat it as live but timing-dependent; the CLI
is `buildtype=debug, optimization=0`, which widens the window.

Also odd: the crashing thread is named `fakefs_-1250`, a NEGATIVE pid.

**Reproduce.** `python3 tests/remote/conductor.py tier0`, then
`./build/ish -f tests/remote/.work/tier0fs-i386 /bin/fakefs_type_race`.
Capture the status directly -- piping to `tail` reports tail's status and hides
the crash.

### eudev does not start on Devuan

**Established.** The message is misleading: sysfs IS mounted. `/etc/init.d/eudev`
line 120 tests for a DIRECTORY, `[ ! -d /sys/class/ ]`, and AOK's sysfs provides
only `block/`, `devices/`, `fs/`.

**The trap.** `log_end_msg 1` does not exit, so the script runs on. Guard 2 asks
whether `ps` shows bracketed kernel threads; it passes today only by accident,
because the one bracketed process is `[elogind-daemon]`. Guard 3 then checks
`[ -e /sys/block -a ! -e /sys/class/block ]` and **sleeps 30 seconds**. So
creating an empty `/sys/class/` makes boot much worse than the current cosmetic
warning. Any fix must supply `/sys/class/block` at the same time.

**Open question.** Whether udevd should run here at all. devtmpfs is advertised
and the app supplies device nodes itself, so disabling the init script in the
rootfs may beat growing sysfs to satisfy eudev.

### SmallCLUE `dmesg` says it is unsupported

**Established.** The implementation already exists: `deps/smallclue/src/core.c`'s
dmesg is a three-way `#if`, with `#elif defined(__linux__)` doing the real thing
via `klogctl(10, ...)`, and the `#else` printing "not supported on this
platform". A native program is compiled as HOST (Darwin) code, so `__linux__` is
undefined and it takes the `#else` -- even though it runs against a Linux guest
whose `sys_syslog` AOK implements and answers (the distro's `/usr/bin/dmesg`
prints AOK's boot line today).

Already done: the stale `dmesg` link is gone -- `native-links.sh` now prunes
links it made before an applet was excluded (`7b35aa49b`).

**Next step.** An `nlibc_klogctl` in the shim issuing the guest syslog(2), plus
a define from `smallclue_c_args` so the klogctl branch compiles for AOK rather
than pretending to be `__linux__`. Then drop dmesg from `EXCLUDED`.

### Uptime and `btime` describe the app process, not the guest

**Established.** `kernel/task.c:579` sets `boot_time = time(NULL)` in
`run_at_boot()` -- once per APP PROCESS. `get_uptime()` reports
`now - boot_time`, and `/proc/stat`'s btime is derived from it. iOS keeps an app
alive across suspensions for days, so when the guest's init restarts inside that
process the guest boots fresh while boot_time still holds the app's launch.
`wtmpdb-update-boot` then refuses it: "Boot time too far in the past".

**This is a decision, not just a fix.** Is the "machine" the app process or the
guest? Uptime-as-app-lifetime is defensible, but Linux userland expects btime to
move when init restarts. If per-guest-boot is the answer, reset `boot_time`
wherever init is (re)started, not only in `run_at_boot()`.

Separately: `platform/darwin.c`'s `get_uptime()` calls
`sysctlbyname("kern.boottime")` into a local and never uses it. Dead code -- and
had it been used it would have reported the DEVICE's boot time, which is worse.

### tmpfs asserts that can abort the whole app

`fs/tmp.c` has five more `assert(S_ISREG(inode->stat.mode))` sites (around lines
206, 451, 475, 1200, 1255, 1287) reachable the way `tmpfs_write`'s was before
`11edc1843`. An assert there aborts every guest process in the app, not just the
caller. Now unreachable via mknod, but the principle stands. Mechanical: give
each an errno appropriate to its caller.

### btop shows nothing in its disk, net and io sections

Reported 2026-08-19. Both procfs files btop reads exist and are populated, so
"the file is missing" is the wrong lead. Each half has its own cause.

**Net -- `/proc/net/dev` prints nine columns a side, not eight.** The header
promises 8 receive and 8 transmit fields and the format string has exactly 16
conversions, but `proc_show_dev()` in `fs/proc/net.c` passes **18** arguments.
Linux's single `frame` column is commented across two source lines there
(`rx_length_errors + rx_over_errors +` / `rx_crc_errors + rx_frame_errors`) and
was given an argument each; `carrier` on the transmit side has the same split.
So every transmit column is shifted one place left and the last two arguments
are dropped on the floor. Visible in any guest:

    lo0: 209946624 302159 0 0 0 0 0 0    29488 209946624 302159 0 0 0 0 0
                                          ^^^^ that is multicast, sitting in
                                               tx_bytes, and rx_bytes has
                                               landed in tx_packets

Fix is two lines: fold arguments 6+7 into one and 16+17 into one, leaving 16.
Worth doing regardless of btop -- the comment above that `proc_printf` records
that ifconfig already needed a workaround for this function's output.

**Disk/io -- nothing ties a mount to a device.** `/proc/diskstats` names the
HOST's device (`disk1` on the Mac CLI), while `/proc/mounts` shows the guest's
root as `alpine-arm64-test / fake`. btop matches mounts to diskstats entries by
device name, and no name in one file appears in the other, so it has nothing to
attach io counters to and lists nothing. Deciding what a guest's disk *is* --
whether the fake fs should present a device name at all -- is a design question,
not a formatting bug, so it is the larger half.

---

## Deferred on purpose

### External display / AirPlay -- GH #540

Work exists on the branch `worktree-external-display-540`:

    6156597ee app: mirror the Wayland display to an external display (GH #540)

**Deferred to a future release by the maintainer (2026-08-18): "the external
display work is flawed".** The commit is NOT merged and must not be swept into a
release by accident. Left on its branch deliberately.

---

## Reported issues

### Bugs

| # | Title | Notes |
|---|---|---|
| [#482](https://github.com/emkey1/ish-AOK/issues/482) | Wayland applet does not resize properly | |
| [#485](https://github.com/emkey1/ish-AOK/issues/485) | Qt apps (Falkon) cannot connect to session bus | 6 comments |
| [#503](https://github.com/emkey1/ish-AOK/issues/503) | amd64: gdb next/step after a breakpoint crashes with SIGILL | ours |
| [#521](https://github.com/emkey1/ish-AOK/issues/521) | Buildroot `make` crashes on "checking for working sigaltstack" | body is a screenshot only |
| [#523](https://github.com/emkey1/ish-AOK/issues/523) | yay (AUR helper) fails on Arch ARM64 | crash fix already pushed (`717e6d3d`); re-test |
| [#527](https://github.com/emkey1/ish-AOK/issues/527) | pikaur fails on Arch ARM64 | blocked on `systemd-run` |
| [#541](https://github.com/emkey1/ish-AOK/issues/541) | ptraceomatic does not run: tracee reaped during setup | ours |
| [#542](https://github.com/emkey1/ish-AOK/issues/542) | JVM/HotSpot crashes on aarch64, "Field too big for insn" | reporter suspects upstream OpenJDK |
| [#558](https://github.com/emkey1/ish-AOK/issues/558) | npm segfault installing OpenClaw | **empty body**; repro requested 2026-08-18, awaiting reply |
| -- | btop shows nothing in its disk, net and io sections | reported 2026-08-19; see *Diagnosed* above |

### Feature requests

| # | Title |
|---|---|
| [#483](https://github.com/emkey1/ish-AOK/issues/483) | Fullscreen mode for the Wayland applet, dynamic resolution |
| [#484](https://github.com/emkey1/ish-AOK/issues/484) | 3D acceleration via virglrenderer |
| [#540](https://github.com/emkey1/ish-AOK/issues/540) | External display support (AirPlay) -- see *Deferred* above |
| [#556](https://github.com/emkey1/ish-AOK/issues/556) | Updated preset appearances |
| [#559](https://github.com/emkey1/ish-AOK/issues/559) | Feedback: own icons rather than iSH's, more OS images, QEMU |
| -- | Terminal cell height / line-height control -- see *Diagnosed* above |

---

## Build and test infrastructure

### Linux CI

**Green again as of 2026-08-19**, both arms of the `[clang, gcc]` matrix.

It had been red since 2026-08-10, which is BEFORE the 548 release -- `e4fe5116`,
the commit tagged 548, was itself red. Never a regression of the 549 cycle, and
it affected no shipped code: `build-mac` and `Build Dev IPA` were green
throughout.

Nearly all of it was one root cause: bash's, zsh's and OpenSSH's `config.h` are
each generated by running configure **on a Mac**, and the tree is compiled for
both platforms, so all three asserted Darwin facts that are false on glibc.
iconv lives inside libc on Linux; `<sys/sysctl.h>` and `<sys/filio.h>` are not
glibc headers; `st_atimespec`, `d_namlen` and `fpurge` are Darwin spellings;
`strtonum`, `timingsafe_bcmp`, `memset_s`, `<util.h>`, the `pw_class` family and
`sin_len` are BSD's. Every such macro is now behind `!__linux__`, so the shipping
build is bit-for-bit unmoved. The rest:

- `__thread` must FOLLOW the storage class for gcc -- 40 declarations, mostly in
  the vendored OpenSSH;
- `-D_GNU_SOURCE` project-wide, for `off64_t`, the `cookie_io` typedefs and
  `RUN_LVL`;
- the xattr port, which was a real port and not a config guard: Darwin's calls
  carry a position and an options word and Linux's do not, so the shim now
  declares the shape each platform actually has;
- `<rpc/types.h>`, which OpenSSH asks for and Debian hides in libtirpc -- one
  missing header accounted for 181 of the original 186 failures;
- the fused i386 ALU gadgets, which exist only in aarch64 assembly, so merely
  naming them was a link error on any x86_64 host;
- a duplicate `smallclueRunRsync`, which ld64 quietly tolerates and GNU ld does
  not.

One of the fixes was not a build fix at all. GCC rejected an assignment clang
waves through and turned up a live crash on iOS: `bash --rcfile FILE` and
`--init-file FILE` wrote through a NULL pointer, and native bash runs in-process,
so that is the app going down rather than a shell. See `deps/bash` `a097512`.

Verified by cloning the pushed branch fresh on Debian 13 and building it exactly
the way CI does, with each compiler: 0 failed targets, `float80` and
`riscv64_decode` pass, `e2e` passes.

### Regression-suite observations

From the 4-arch on-device run, 2026-08-19 (aarch64 booted 118/118 clean; i386
110/6, x86_64 112/5, riscv64 103/5):

- **Five failures are identical on every chroot arm and absent from the booted
  arm**: `devtmpfs_mount`, `proc_pid_io`, `taskstats_genl`, `mount_stdev`,
  `fifo_open_creat_deadlock`.
- `mount_stdev` and `devtmpfs_mount` are proven chroot artifacts: they fail in
  an **aarch64** chroot, the same arch that passes them when booted. AOK has no
  mount namespaces, so `/proc` inside a chroot describes the booted root while
  `stat()` sees the chroot's.
- The other three pass when run **by hand** inside the same chroot, and failed
  when x86_64 ran **alone**, so it is the chroot plus the suite runner -- not
  contention and not architecture. Worth understanding before anyone reads them
  as product bugs.
- `mount-root.sh` bind-mounts `/AOK/tools` but not `/AOK/tests`, so
  `setup-regressions.sh` cannot find its sources inside a chroot without a
  manual bind. Small gap worth closing if this becomes routine.
