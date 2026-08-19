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

### tier0: two pre-existing i386/x86_64 failures, unrelated to the JIT fix

Found by sweeping every tier0 binary on both roots while checking the JIT fix
(2026-08-19). Both were verified against a pre-fix binary and behave identically,
so neither is a regression -- recorded because nothing else names them.

- **`pread_stack_thread_race` hangs on i386**, 3 of 5 runs; before the JIT fix
  the same test hung once and took SIGSEGV once, so the fix removed the crash
  but not the hang. Passes on x86_64. Next step is a `sample(1)` on a hung one.
- **`statx_mnt_id_timerfd` fails on x86_64**, `failures=6`, identically before
  and after. It passes in the arm64 guest suite, so this is amd64-specific.

Also, `fakefs_casefold` does not clean up `/tmp/fakefs_casefold.1`, so a second
run in the same root fails on "File exists". A test-hygiene bug, not a product
one, but it makes any repeated tier0 sweep report a false failure.

### btop shows nothing in its disk and io sections

Reported 2026-08-19. Both procfs files btop reads exist and are populated, so
"the file is missing" was the wrong lead. **The net half is fixed** -- see
*Closed during the 550 cycle* below. This is the remaining half.

**Nothing ties a mount to a device.** `/proc/diskstats` names the HOST's device
(`disk1` on the Mac CLI), while `/proc/mounts` shows the guest's root as
`alpine-arm64-test / fake`. btop matches mounts to diskstats entries by device
name, and no name in one file appears in the other, so it has nothing to attach
io counters to and lists nothing.

**This is a design question, not a formatting bug.** Deciding what a guest's
disk *is* -- whether the fake filesystem should present a device name at all,
and if so whether one name per fakefs root or one for the whole guest -- has to
be settled before any code. Whatever is chosen must appear in BOTH files under
the same name, which is the invariant btop actually depends on.

---

## Closed during the 550 cycle

### i386 `fakefs_type_race` killed the CLI build -- FIXED 2026-08-19

**The recorded bisect was measuring the wrong thing.** The entry said
`ISH_I386_NOCHAIN=1` made it pass and `ISH_I386_NOBACKCHAIN=1` did not, and
concluded "forward-edge block chaining". But `i386_jit_chaining_enabled()` is a
C-side gate on the *linking loop* only -- it does not touch a single line of the
assembly that writes `frame->last_block` -- and it appears in the crashing
expression itself:

```c
if (last_block != NULL && i386_jit_chaining_enabled() &&
        (last_block->jump_ip[0] != NULL || ...     // <- jit.c, the faulting line
```

`&&` short-circuits, so `NOCHAIN=1` was not preventing the corruption, it was
skipping the DEREFERENCE. Instrumenting `last_block` proved it: with
`NOCHAIN=1`, and the test "passing", the pointer was still being corrupted
1-3 times per run. Chaining was never involved.

**What it actually was.** `frame->last_block` was `0x4`, and `jump_ip` is at
offset `0x18`, giving the reported fault address `0x1c`. 4 and 8 are `4 + imm`
from `RET_NEAR(imm)` -- a **ret** gadget's pop count. The `ret` gadget's
return-cache path reads its candidate block pointer from the offset where a
**call** gadget keeps one, so the cache entry was pointing into a block that had
been freed and whose memory had been reused by a block with a ret gadget there.

The dangling entry survives because of a hole between the two staleness guards:

- `jit_entry_scratch_get()` purges `cache` and `ret_cache` when
  `jit_block_free_generation` has moved -- but it is called **before** the
  caller takes `jetsam_lock`, so it samples the counter too early;
- the frontend loop purges when `cleanup_seq` has moved -- but it seeds
  `last_block_cleanup_seq` **after** the lock, so a bump inside the window is
  already included and never seen.

A `jit_free_jetsam()` pass landing between the two reads is therefore invisible
to both, and the thread runs the whole entry with a `ret_cache` full of pointers
into freed blocks. Fixed by `jit_entry_scratch_refresh()`, called with the read
lock held -- at which point no free can be in flight, so the generation it reads
holds for the entry. One relaxed load when nothing was freed; no measurable
cost on a syscall-heavy or a find-heavy guest benchmark.

**All four frontends had it**, not just i386 -- the same three lines appear in
the arm64, riscv64 and amd64 entry paths, and all four are fixed. i386 is
simply where a test hit it: 3/3 crashes before, 6/6 clean after, and 9/9 across
`default` / `NOCHAIN` / `NOBACKCHAIN` with the instrumentation still in and
reporting zero corruption events.

`ISH_I386_NOCHAIN=1` is no longer needed as a workaround, and would not have
been a real one -- it left the corrupt pointer in place.

**Regression gate:** `python3 tests/remote/conductor.py tier0`, then
`./build/ish -f tests/remote/.work/tier0fs-i386 /bin/fakefs_type_race`. Capture
the status directly; piping to `tail` reports tail's status and hides a crash.

### `/proc/net/dev` printed nine columns a side -- FIXED 2026-08-19

`proc_show_dev()` passed eighteen arguments to a format string with sixteen
conversions: Linux folds four counters into its single `frame` column and four
more into `carrier`, and the port had given each half of those sums an argument
of its own. printf dropped the last two and every transmit column sat one place
left of its header -- multicast printed as tx_bytes, rx_bytes as tx_packets.
btop's network panel was empty and ifconfig showed a loopback that had received
259 MiB and sent 33 kB. Fixed in `104e5ff4d`.

**Why it shipped, and what else it was hiding.** `proc_printf()` carried no
`format` attribute, so no compiler ever checked a single procfs format string.
It has one now, and it immediately found three more:

- `/proc/<pid>/stat` printed six `addr_t` values through `%lu`. `addr_t` is 32
  bits, so the conversion read eight bytes where four were passed -- correct
  today on Darwin arm64 only because the adjacent stack happens to be zero.
- `/proc/<pid>/status` rendered the signal masks as `%08x` from a 64-bit
  `sigset_t_`; Linux's `render_sigset_t()` always emits all sixteen hex digits.
- `/proc/consoles` passed `console_major` twice and dropped `console_minor`.

**Guarded by `tests/manual/proc_field_layout.c`.** Counting fields does NOT
catch the `/proc/net/dev` bug -- the line still had sixteen of them -- so the
test pushes a megabyte through loopback and requires the column the header
calls tx_bytes to move by at least that much. Against the pre-fix kernel it
reports tx_bytes moving by 0 and tx_packets by 1054720, which is rx_bytes,
exactly one place over. Passes on real Linux (Debian 13, 6.12) too.

Not simplified, deliberately: the literal space after the name colon. It is
what Linux itself writes, not a workaround, and without it an 8-digit rx_bytes
glues onto the interface name and busybox ifconfig loses the device. The test
asserts it so nobody "tidies" it away.

### Lingering `ish` processes: the `fflush(NULL)` deadlock -- FIXED 2026-08-19

The entry that used to sit under *Diagnosed, not fixed* had the stack right and
the mechanism half right, and the fix it proposed would have traded the hang for
silent data loss. Recorded here because of that.

**What was actually observed**, with `lldb` on two live wedged processes rather
than inferred:

- The blocked frame is `cli_halt -> _fwalk -> sflush_locked -> flockfile ->
  __psynch_mutexwait`. `_fwalk` is `fflush(NULL)` walking every host stream.
- The stream it blocks on is **one of ours**: `_file = -1`, `_write =
  nlibc_file_write`, cookie = guest fd 1. That is the native-libc shim's
  `funopen()` wrapper for a native program's stdout (`nlibc_file_wrap`), living
  in libc's `usual[]` pool -- NOT `stdout`, and not some stream a native program
  opened for itself.
- The mutex's recorded owner tid was absent from the process's own live thread
  list, in both processes. Both had exactly two threads left, neither of them
  the owner.

So: a native program is host code on a guest task's thread. A task killed before
`nlibc_flush_std()` runs leaves its wrapped stdout live in libc's pool, and a
task killed INSIDE stdio leaves that stream's lock held. Darwin does not release
a mutex when its owner thread dies, so `fflush(NULL)` waits for a thread that no
longer exists. Confirmed independently by a 30-line host probe with no AOK in
it: a thread that exits holding a `flockfile` makes a later `fflush(NULL)` hang
for ever, and `ftrylockfile` refuses that lock rather than blocking on it.

**Why the proposed fix was wrong.** "Flush `stdout` and `stderr` by name" would
have stopped the hang, but `stdout` is `__sF[1]` -- not where a native program's
pending output is. The shim's wrappers are, and skipping them loses the tail of
every native program's output. What landed instead: flush the same set of
streams, each guarded by `ftrylockfile()`, and skip any whose lock cannot be
taken. A stream nobody can lock has nothing recoverable in it anyway. Verified
A/B on the real algorithm -- old hangs, new returns and still writes the owned
stream's bytes -- and by 360 runs of the reproducer under self-contention with
zero survivors.

**The same landmine was inside native programs, and that one reached users.**
`fflush` was on `check-native-libc.py`'s PURE list, justified as "every stream a
native program holds is one the shim made". True of `fflush(f)`, and not an
argument about `fflush(NULL)` at all -- that one flushes EVERY stream in the
process, meaning every other concurrently-running native program's stdout and
stderr as well, and it blocks on any lock a departed task's thread still holds.
smallclue's shell alone calls it 20 times, several around fork and pipeline
teardown. This is the third instance of that exact error shape, after `fileno`
and `getopt`: premise right, conclusion backwards. `fflush` is now routed to
`nlibc_fflush`, which reads NULL as "the streams this program owns" -- the
registry gained a per-thread owner tag to answer that -- and flushes them
without waiting. Unlike the `cli_halt` half, this one was reachable in the iOS
app, where it would have hung a user's shell rather than a test process.

`fclose` came with it, for a different reason. It was on PURE too, and closing a
stream really does reach nothing on the host -- but it has to drop the stream
from the shim's own registry, and leaving it to the host left a stale entry
behind for every `fopen`/`fclose` pair a native program ever made. Only
`nlibc_flush_std`'s teardown forgot one. The registry is keyed by `FILE*` and
libc reissues the same slot, so a later stream inherited the dead entry's answer
to `fileno()` -- the hazard `nlibc_stream_forget`'s own comment describes, with
nothing closing the loop. Now routed to `nlibc_fclose`, and `nlibc_freopen` and
`nlibc_pclose` go through it too.

Also fixed in passing: refreshing the archives the gate reads (`ninja ish` does
not, which is its own trap) exposed a real pre-existing failure --
`deps/zsh/Src/aok_fork.c` calls `pthread_get_stackaddr_np` and
`pthread_get_stacksize_np`. Those ask about the calling HOST thread's own stack,
which is the only correct answer for a stack-overflow guard, and the shim's own
`nlibc_stack_exhausted()` asks the same two questions. Added to PURE with that
reason; the gate is clean again, at 236 host symbols.

---

## Closed during the 549 cycle

Kept only because the entry was wrong in a way worth remembering.

### tmpfs asserts that can abort the whole app -- FIXED 2026-08-19

This entry said the remaining sites were "now unreachable via mknod". That was
wrong, and the cost of being wrong was high: `11edc1843` mapped only the type-0
case, so any OTHER invalid `S_IFMT` -- `0x3000`, say -- walked through
`generic_mknodat`, which rejects only DIR and LNK and gates only BLK and CHR on
superuser. The result was a tmpfs inode of no type at all, and `read`, `pread`,
`pwrite` and `ftruncate` each hit an assert and aborted the WHOLE app. Three
lines of C, no privilege required, on any mounted tmpfs. Reproduced, then fixed:

- `kernel/fs.c` now whitelists the five types Linux's `may_mknod` accepts and
  returns EINVAL otherwise, so such an inode cannot be created in the first
  place;
- the five reachable asserts in `fs/tmp.c` became errno returns anyway, because
  an assert reachable from a syscall argument is the wrong tool.

The one assert left (`tmpfs_init_regular_file`) is a creation-time invariant its
only caller satisfies by construction.

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
| -- | btop shows nothing in its disk and io sections | reported 2026-08-19; net half fixed, disk half is a design question -- see above |

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
