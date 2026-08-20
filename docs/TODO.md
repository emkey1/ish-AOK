# iSH-AOK TODO

Open work: bugs that are diagnosed but not fixed, reported issues, and features
deferred on purpose. Each entry says what is already **established**, so nobody
re-derives it, and what the **next step** actually is.

Started 2026-08-19, after the 549 release run.

---

## Diagnosed, not fixed

### AOK loses a connected UDP socket's error about a third of the time

Found 2026-08-19 while building the regression test for the chronyd spin, and
older than that fix. A connected UDP socket that takes an ICMP port-unreachable
should report ECONNREFUSED to the next recv. AOK reports it **14 times in 20**;
the macOS host underneath reports it 20 in 20, and Linux 6.12 delivers 10 of 10
running this test's own UDP half (AOK manages 9 of 10 on a good run).

**Established.** When it does arrive it is always on the very first poll, so
this is presence-or-absence, not slowness -- three seconds of polling does not
recover a lost one. Ruled out by measurement: the port is genuinely free after
the probe socket closes (0 of 20 still held, same as the host), so the datagram
is not being delivered to a lingering listener instead of refused.

**Why it matters.** It is the same family as the spin that has just been fixed:
a poll loop that never learns its socket is dead. `chronyd` is exactly such a
loop.

**Reproduce.** `tests/manual/sock_conn_error.c` logs the ratio on every run
("udp-unreachable delivered 9 of 10 attempts"). The test deliberately does NOT
fail on a single miss -- a flaky assertion teaches people to ignore the suite --
so it requires only that some attempt out of ten deliver.

**Next step.** Find what consumes it. The obvious candidate, AOK's own
`socket_tcp_connect_write_ready()` reading the host's read-and-clear SO_ERROR
from inside `sock_poll` (which really was eating the equivalent error on TCP,
now fixed), returns early for anything that is not SOCK_STREAM, so it is not
this. Start by tracing whether the host ever reports it for the losing runs.

### `pread_stack_thread_race` hangs, on both x86 arches

**Established.** Hangs 3 of 5 runs on i386, and hangs on x86_64 too -- it was
the ONLY failure in the final tier0 sweep (i386 106 passed / 0 failed / 4
skipped; x86_64 105 / 1 / 4), so it is not arch-specific as first recorded. Before the JIT
ret_cache fix (`49de7e671`) the same test hung once and took SIGSEGV once in two
runs, so that fix removed the crash and not the hang. The conductor's timeout is
180 seconds and the test's own work is about 8, so this is a real hang rather
than slowness.

The test is a deliberate stress repro: 6 pread-into-own-stack workers, 3 mmap/
munmap churn workers and 2 fork/wait workers, all on one shared address space
for 8 seconds. Its header says a hang or a crash here is the confirming signal
for the suspected `mem_ptr()`-vs-concurrent-address-space-mutation race it was
written to catch, so this is the test doing its job.

It is tier0-only -- not in `all_tests` and not in `fs/aok-tests.manifest` -- so
it runs under `python3 tests/remote/conductor.py tier0` and nowhere else.

**Next step.** `sample(1)` a hung one. See [[go-runtime-concurrency-debugging]]
for the lldb setup, and note the `process handle SIGUSR1` lines are needed
before `run` or lldb stops on AOK's own poke signal.

### `pidfd_open` refuses a zombie, so `pidfd_epoll_deadlock` fails 1 run in 3

Found 2026-08-20 in a tier0 sweep: x86_64 came back 104 passed / 2 failed where
the recorded baseline is 105 / 1, the extra failure being

    pidfd_epoll_deadlock: FAIL pidfd_open: No such process

Re-run alone it fails 1 of 3 on x86_64 and passes every time on i386, so it is
a race rather than an arch difference.

**Established.** It is not the test. The test forks a child that `_exit(0)`s at
once and then calls `pidfd_open(child)` before waiting -- deliberately, because
the exit is half of the race it exists to catch. At that moment the child is a
zombie, and on Linux `pidfd_open` on a zombie SUCCEEDS: the fd is immediately
readable, which is how a pidfd reports an exit at all. ESRCH is Linux's answer
only for a pid that does not exist, meaning after reaping.

AOK returns ESRCH because `sys_pidfd_open` (kernel/pidfd.c:103) calls
`pid_get_task_ref`, and `pid_get_task` (kernel/task.c:75) filters zombies out
by design:

    struct task *task = pid_get_task_zombie(id);
    if (task != NULL && task->zombie)
        return NULL;

So the test only fails when the child wins the race to exit before the parent's
`pidfd_open` -- which is exactly the flakiness observed, and exactly what the
test was written to provoke.

**Next step.** `pid_get_task_zombie` is right there and is what this call wants.
Before switching to it, check the rest of the pidfd machinery against a zombie
task: `pidfd_create` takes a reference, and do_exit's first step busy-waits for
pidfd references to drop (see the O_CLOEXEC comment in the same function), so a
pidfd opened on an already-zombie task must not be able to wedge the reaping it
is waiting for. Reading a zombie's pidfd should report ready immediately, and
`waitid(P_PIDFD, ...)` on one should reap normally -- both worth a test
alongside the fix.

### eudev refuses to start: "does not support containers" (Devuan)

Reported 2026-08-20, on Devuan. Left over from the sysfs work below: with
`/sys/class/block/sda` supplied, eudev's init script gets past both the "sysfs
not mounted" complaint and the 30-second guard, and stops at the NEXT check
with

    eudev does not support containers, udevd not started ... (warning)

exit 0, in under a second. That was recorded as eudev deciding correctly on its
own, and as a boot that is no longer slow it is an improvement -- but the device
manager still does not run, so this is a warning standing in for a feature that
is simply absent.

**Established.** The message is eudev's own container detection, not AOK's. It
is a guard in the init script rather than in udevd, and it is reached only
because everything before it now passes. The Alpine path is different -- Alpine
ships mdev, which is on native-links.sh's EXCLUDED list -- so this entry is
about Devuan and any other eudev distro.

**Next step.** Read what eudev's container check actually tests: on a systemd
system it is `/run/systemd/container` and the `container=` environment
variable, and eudev's is a variant of that. Decide between three answers, in
this order of preference:

1. AOK is not a container and can say so -- if the check is a file or an
   environment variable AOK sets or inherits by accident, stop setting it.
2. It IS container-like by that definition and udevd genuinely cannot work
   (it wants netlink uevents, which AOK's kernel does not generate). Then the
   honest fix is documentation plus, if a guest needs device nodes, keeping the
   existing `/dev` repair doing that job -- and the warning is correct.
3. Neither: make udevd start and watch it fail, which is worse than the
   warning.

Answering 1 vs 2 is the whole task, and it is a read of eudev's source rather
than of AOK's.

## Closed during the 550 cycle

### `md`'s word boundaries come from the HTML, not from letter case -- FIXED 2026-08-20

The camelCase splitter behind the original "i SH-AOK" report was a guess
standing in for information the HTML converter had thrown away: dropping a tag
without putting anything in its place runs words together, so
"<td>Some</td><td>Text</td>" became "SomeText", and the answer had been to
insert a space wherever a lowercase letter met an uppercase one. Gating it to
fetched pages stopped it damaging local documents; it still damaged the fetched
ones, where macOS read as "mac OS" and GitHub as "Git Hub".

markdownHtmlTagKind classifies every tag reaching the converter's default case
-- inline, side by side, own line, own paragraph -- so the boundary comes from
the element, which is the only place it exists. <td>, <button>, <dt>/<dd>,
<nav> and the rest separate; <b>mac</b>OS stays "macOS".

That left the guessing with nothing to do, so it is gone along with its three
helpers, an 8 KB stack buffer and a copy of every line. It had been doing more
than letter case: splitting a digit from a letter ("utf8mb4"), splitting
"array[0]", and carrying hardcoded fixups for particular scraped sites.

**The limit, recorded so it is not filed again as a bug.** CSS can make an
inline element a block one and this does not read CSS, so GitHub's navigation
-- two <span>s inside one <a> -- still runs together as "GitHub CopilotWrite
better code with AI". Unfixable from the element name, and the better failure:
wrong about navigation furniture on some pages rather than wrong about macOS in
every document. Article text on the same page reads correctly throughout.
`6834e9e` in the fork.

### `md`: indented code blocks, and the syntax that showed through -- FIXED 2026-08-20

The indented-code-block gap filed earlier is closed, and the trap it was filed
over turned out to be real: four spaces under a bullet is that item's
continuation paragraph, not code. Detection is guarded on not being in a list
and on no paragraph being open, since an indented chunk cannot interrupt one.

Fixing it surfaced more of the same kind:

- **A list item's own wrapped lines were rendered as a separate block**, so the
  first line wrapped to the margin and the rest started again underneath --
  stray one-word lines like "    but". Items go through the paragraph buffer
  now and wrap once.
- **Width was measured in bytes.** "• " is three bytes and one column, and the
  list prefixes compensated by indenting continuations two spaces too far.
- **Every list rendered loose**, once items went through the paragraph buffer,
  until the trailing blank was made to come from a blank line in the source.
- **`has_blank_separator` was lying after a heading** -- it claims the output
  ends with a blank line and none was emitted, so two headings ran together.

And five inline constructs were printing their own markup: `\*` kept the
backslash and lost the asterisk; `&amp;` and numeric references rendered
literally; `![alt](url)` came out as "!alt [3]"; `<https://example.com>` kept
its brackets; and `[text][ref]` showed its brackets while every "[ref]: url"
definition was rendered as a paragraph, so a README keeping its links at the
bottom ended with a block of bare URLs. Hard breaks (two trailing spaces) are
honoured rather than reflowed away.

Checked against all 14 documents: every word of every source still present, no
colour-marker leaks in piped output, and the only over-wide lines are inside
code blocks. `11f07b4` in the fork.

### `md` rewrote the documents it rendered -- FIXED 2026-08-20

Reported against /AOK/docs, where "iSH-AOK" rendered as "i SH-AOK". Three bugs,
all of them the renderer editing text it should have been showing, and the
reported one was the least damaging of them.

- **Web-scrape heuristics ran on local files.** md cleans up pages fetched as
  HTML, and every one of those passes also ran on real .md files. camelCase
  splitting rewrote the whole vocabulary -- macOS to "mac OS", GitHub to "Git
  Hub", NSURLSession to "NSURL Session" -- and the CSS-selector detector
  DELETED whole sentences: two commas and a '.' not followed by a space was
  enough, so "small, synthetic, read-only filesystem (`aokfs`, see `fs/aok.c`)
  that" vanished from the overview with no marker where it had been. They are
  gated on the input having come out of the HTML converter now.
- **Code spans were not verbatim.** A backtick was skipped rather than opening
  a span, so `a*b*c` rendered as "abc".
- **Every '*' and '_' was deleted on sight**, matched or not: TZ_NAME to
  TZNAME, kernel/native_libc.c to kernel/nativelibc.c, "5 * 3" to "5  3".

Checked by rendering all 14 documents and diffing every word against the
sources: 66 words were missing before, and the remainder are table-cell
wrapping and '/'-joined tokens, verified present by hand. `787236a` in the
fork, `bdbd617e0` here.

### `md` has colour -- ADDED 2026-08-20

The reason it had none was a good one: the pager sanitises every line so a
fetched page cannot emit escape sequences. So the renderer emits a two-byte
private marker instead, and the pager expands markers into SGR only for buffers
it was told are markdown renders. A document carrying a stray marker can change
a colour and nothing else -- no cursor movement, no screen clear, no mode
switch. Markers are only emitted when something will expand them, so a
redirected run is plain text; NO_COLOR and a dumb TERM are honoured.

Two fixes fell out of it. **The pager's `raw_mode` had never worked**: it was
assigned before pagerCollectLines, which memsets the struct, so `less -r` has
been sanitising its output for as long as the option has existed. And **table
cells printed their backticks** while the same code span in a paragraph did
not, because cells never went through the inline pass. `87e628c` in the fork.

### The applets stubbed over a missing library -- FIXED 2026-08-20

tar, gzip, gunzip, zcat, curl, wget and `md <url>` all refused with "not built
into this iSH-AOK" or "networking support is unavailable in this build". Both
groups are real now, and neither was the build-system problem it looked like.

- **zlib was never missing.** It is public on iOS as well as macOS. The actual
  obstacle was that a system dylib compiled against the HOST libc does host
  I/O: `gzopen` on a guest path opened iOS's copy, invisibly correct wherever a
  path existed on both sides. deps/smallclue-shim/zlib.h reimplements the six
  gz* calls over the redirected open/read/write and leaves deflate/inflate --
  which touch nothing but the caller's buffers -- to zlib. `550bc653c`.
- **libcurl really is absent from the iOS SDK**, header and tbd alike.
  deps/smallclue-shim/curl/curl.h implements the seven functions core.c uses on
  NSURLSession. That is host networking, deliberately and with the cost written
  down: guest /etc/hosts and /etc/resolv.conf do not apply. `643c8ef7d`.
- **Enabling tar exposed a corruption in it.** SmallCLUE's tar treated GNU
  @LongLink and PAX headers as files, then read their data as the next header
  and abandoned the archive -- and those are what busybox tar writes for any
  path over 100 bytes. Fixed in the fork before tar could reach anyone's PATH.
  `07ed79c6e`.

native-links.sh needed no edit: its PROBED list already anticipated "tar and
gzip need zlib, curl and wget need libcurl" and runs each applet once to ask.
The link count went from 106 to 112 on its own.

**Verified on device** (ipp4-dev-arm64, an M4 iPad, 2026-08-20): tar, gzip,
gunzip, zcat, curl, wget and `md <url>` all work in the app, which settles the
`libz.tbd` link -- a build that had missed it could not have launched. It could
NOT be confirmed by a headless build here, for reasons worth knowing separately:
the project's targets do not build standalone. `libiSH-AOKApp` resolves SDKROOT
to macOS and fails on `UIKit/UIKit.h`, and `iSH-AOK.FileProvider` links
`-lish_emu` before the Meson target has produced it, because neither declares
the dependency that Xcode's scheme-driven build infers. Both are pre-existing.

**The device found one thing the CLI could not**: plain HTTP to a public host
failed in the app while HTTPS and localhost worked -- App Transport Security's
exact signature, confirmed by serving plain HTTP from the guest itself and
watching that succeed while `http://example.com` failed and the emulated curl
got 200 from the same device. app/Info.plist declared NSAllowsArbitraryLoads
AND NSAllowsLocalNetworking, and the presence of the latter makes the former be
ignored on iOS 10 and later. Removing it is what makes the declaration apply:
confirmed on the rebuilt device, where three plain-HTTP hosts now fetch, HTTPS
is unaffected, and localhost still works -- so nothing was lost with the key.
The shim also now reports the framework's own wording instead of mapping every
unrecognised NSError to "Failure when receiving data from the peer", which is
what made this look like a network fault for as long as it did.

### Terminal cell height -- FIXED and SEEN 2026-08-19

At font sizes under 16 a background highlight sat 1-2px proud of the Powerline
separator beside it. hterm sizes a cell as
`fontBoundingBoxAscent + fontBoundingBoxDescent` -- the font's MAXIMUM extent,
with room for accents and deep descenders a block glyph never uses. The
background fills that cell; `U+2588` only rises to about the em height, and the
leftover is the band.

Fixed with a `line-height` MULTIPLIER (not the reporter's second suggestion, a
baseline offset, which cannot fix a glyph shorter than its cell -- it only moves
the band from the top to the bottom). A multiplier rather than a formula because
how much of the em a patched font's blocks cover varies between Nerd Font
patches. Default 1 is the measured height, so nothing moves until someone asks.
Settable at `/proc/ish/defaults/line_height` and from a Line Height row under
Font Size in Appearance.

**Seen, on an iPhone 17 Pro simulator, at font size 12** -- five rows of `U+2588`
on a red background, three rows of blue background-only, and a line each of
descenders/accents and CJK:

| line-height | block-glyph band | `Agjpqy ÀÉÎÕÜ` and CJK |
|---|---|---|
| 1.00 (default) | thick red band between every row | intact |
| 0.85 | much thinner | intact |
| 0.75 | thinner still | **descenders clipping** |

So the useful range for that font is about 0.85-0.95, the band narrows rather
than vanishing, and clipping starts below ~0.8 -- which is exactly why this is a
knob with bounds and not a computed value. Background-only rows tiled solid at
every setting, confirming the diagnosis that it is the glyph and not the cell.

**What actually took the time, and the real bug behind it.** The JS was correct
from the start and had no effect for three builds, because
`app/terminal/term.html` loads `hterm/dist/js/hterm_all.js` -- a bundle that is
gitignored and that NOTHING in the build produced. The phase named "Compile
JavaScript" only asserted the file existed. Fixed in `61fc0f59a`; see that
commit for why the regeneration lives in `xcode-meson.sh` and not in the phase.

### SmallCLUE `dmesg` said it was unsupported -- FIXED 2026-08-19

The implementation was there all along, behind `#if defined(__linux__)`, and a
native program is compiled for the HOST -- so the test was false even though the
kernel the call would reach is Linux, and AOK's own. The same guest's
`/usr/bin/dmesg` printed the boot line perfectly while this said it could not.
The platform test was asking about the compiler's target when what matters is
which kernel answers.

`nlibc_klogctl` issues the guest's `syslog(2)`; `deps/smallclue` gained
`SMALLCLUE_HAVE_KLOGCTL` so the existing branch compiles; and dmesg left
`native-links.sh`'s EXCLUDED list, which now skips 27 applets rather than 28.
Output verified byte-identical to the oracle -- the same guest's `/bin/dmesg` --
for both `dmesg` and `dmesg -T`. `42c1536da`.

### eudev's "sysfs not mounted", and the 30-second sleep behind it -- FIXED 2026-08-19

`/sys/class` did not exist at all. Fixed together with the block-device naming
below, because they need the same thing: `/sys/class/block/sda`.

Supplying `/sys/class` alone would have made boot **worse**. `log_end_msg 1`
does not exit, so the script runs on to a guard that checks
`[ -e /sys/block -a ! -e /sys/class/block ]` and sleeps 30 seconds; an empty
`/sys/class` turns a cosmetic warning into half a minute on every boot. Both
arrive together and both guards now pass.

**The open question answered itself.** Run against a real Devuan root with eudev
actually installed, the init script no longer complains about sysfs, does not
sleep, and stops at its NEXT guard with "eudev does not support containers,
udevd not started ... (warning)", exit 0, in under a second. eudev decides
correctly on its own. On a root where that container check passes by accident
(the TODO's note about `[elogind-daemon]` being the one bracketed process), the
30-second guard is now passed too, so both paths are covered. `4fb8c0768`.

### btop's empty disk, net and io panels -- FIXED 2026-08-19 (in two goes)

**The first attempt fixed the wrong thing, and this is why.** The entry said
btop matches mounts to diskstats entries by device name, so the two files were
made to agree on `sda`. Verified: the invariant held, in every file, on all four
guest arches. Then it was installed on an iPad and both panels were still empty
-- because that invariant was never what btop reads.

Reading btop's own binary settles it in one command:

    strings /usr/bin/btop | grep -E "^/(proc|sys|etc)/"
    /etc/fstab          /etc/mtab          /sys/block/{}/stat
    /sys/class/net/     /statistics/       ...

`/proc/net/dev` does not appear ANYWHERE in it, and `/proc/diskstats` does not
either. Two separate causes, neither of them column alignment:

- **Disks come from `/etc/fstab`.** btop's `use_fstab` defaults to true, so its
  whole disk list is whatever fstab declares -- and a rootfs tarball has never
  heard of AOK's root. Alpine's ships `noauto` lines for a CD-ROM and a USB
  stick and nothing else. Measured both ways: with `use_fstab = false` the panel
  fills in; with the default, blank. `ensure_root_fstab_entry()` (kernel/init.c,
  called from both the CLI and the app at boot, like the /dev repair beside it)
  adds a root line when nothing declares "/", and only then.
- **Counters come from `/sys/class/net/<iface>/statistics/`.** AOK had no
  `/sys/class/net` at all, so every interface read zero -- which is exactly the
  "shows no traffic" seen on the iPad's tailscale interface. Now present, with
  statistics plus address/mtu/flags/operstate/carrier/type, built from the same
  snapshot `/proc/net/dev` uses so the two cannot disagree.

With btop's stock config, unmodified: disks show 926 GiB at 32% used, and the
net panel reads 2.88 MiB/s down / 124 KiB/s up against real traffic.

**The lesson, since it cost a round trip:** the invariant was verified and the
outcome was not. Running btop once would have caught it in a minute, and reading
its binary for the paths it opens would have caught it before any code changed.

**The name fix was still worth doing**, for everything that DOES read those
files. `/proc/diskstats` named "disk1" and `/proc/mounts` named the root
"alpine-arm64-test", so nothing tied a mount to a device. "disk1" was the host's name for a Mac
disk -- a host detail leaking into a guest -- and it contradicted the major 8,
minor 0 printed beside it, which IS sda in Linux's numbering.

The device is `sda` now, defined once in `fs/real.h` and used by
`/proc/diskstats`, `/sys/block` and `/sys/class/block`, and `/` reports
`/dev/sda`. That replaces a deliberate choice worth naming: `mount_root` used to
report the root's directory name there so df would not print the host path.
`/dev/sda` is the Linux answer and the one that makes tools work; busybox df
prints it correctly. `4fb8c0768`.

### Uptime and btime described the app process -- FIXED 2026-08-19

The decision the entry asked for: **the machine is the guest**. `boot_time` is
now set where pid 1 is created, the only event in AOK that means what a boot
means, instead of once per app process. `run_at_boot` still seeds it so nothing
reading the clock before init exists sees zero.

Two more found there. `sysinfo(2)` reported uptime in the wrong unit --
`uptime_ticks` is 100 Hz and `kernel/uname.c` handed it to a field measured in
SECONDS, so a 12-second-old guest read as "up 20 min" through busybox uptime and
anything else on `sysinfo(2)`. And the two platforms disagreed about that unit,
which is how it survived: darwin produced ticks, linux produced seconds taken
straight from the HOST's `sysinfo()`, a different and much older machine. Both
produce ticks from the guest's boot now. Measured after a `sleep(15)`: sysinfo
16 s, `/proc/uptime` 16.0 s.

Also gone: darwin's `get_uptime()` read `kern.boottime` into a local and never
used it -- and had it been used it would have reported when the DEVICE last
booted. `f23d92bdc`.

Confirmed end to end on the Linux build, where the old behaviour was worst: on a
host 685417 seconds into its uptime, a guest that had slept 6 seconds reports
`/proc/uptime` 7.0 and a btime 7 seconds back. GCC and clang both build it
clean; `platform/linux.c` needed a `<time.h>` it had been getting from nobody
(`d672344af`).

### The `fflush(NULL)`-adjacent socket bugs -- chronyd's spin -- FIXED 2026-08-19

Two bugs, one recorded and one not.

**The recorded one.** iOS kills connected sockets when the device sleeps, reads
return ENOTCONN, and `sock_translate_err` maps that to ECONNRESET -- on every
call, for ever, because the host keeps answering ENOTCONN while `sock_poll` went
on reporting the fd readable. The translation now records that the connection is
finished; after that, reads report end-of-file and poll reports
`POLL_ERR|POLL_HUP`.

**Correction to the plan that was written here.** `POLL_ERR|POLL_HUP` alone
would NOT have stopped it. `kernel/poll.c`'s `SELECT_READ` counts both as
readable, matching Linux, so a select-based loop like chronyd's still wakes. It
is the read returning EOF that ends the loop; both halves are needed.

**The unrecorded one, and the one that could be measured.** A TCP peer resetting
with SO_LINGER 0 makes `recv()` report ECONNRESET on the macOS host and on Linux
6.12 alike. An AOK guest agreed -- until it called `poll()` first, after which
the same `recv()` returned 0 bytes. AOK reads the host's SO_ERROR itself
(`socket_tcp_connect_write_ready`, on every `sock_poll` of a stream socket) and
SO_ERROR is read-and-clear, so AOK's own poll consumed the pending error and the
guest's read saw a clean end-of-file. The stash that already existed for this
was consulted only by `getsockopt(SO_ERROR)`; `read`, `recvfrom` and `recvmsg`
consult it now too.

**The secondary item in the old entry did not reproduce.** It said a connected
UDP socket on ICMP port-unreachable showed ECONNRESET where the host and Linux
show ECONNREFUSED. AOK reports ECONNREFUSED correctly. What it does do is lose
the error entirely about a third of the time -- filed above, on its own.

Guarded by `tests/manual/sock_conn_error.c`, which passes on real Linux 6.12 as
well as on AOK -- re-run against the oracle after it came back online, together
with `proc_field_layout`. `31261988b`.

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

## Native program candidates

Programs worth compiling in as native code (kernel/native.h), and the one
question that decides most of them.

**The dividing line is the shim, not the program.** kernel/native_libc.h works
by `#define`-ing libc names ahead of the system headers, so it redirects calls
in translation units AOK COMPILES. It does nothing to calls made from a
prebuilt dylib or from another toolchain's objects -- that is exactly what made
zlib's gz* family unusable until deps/smallclue-shim/zlib.h reimplemented it.
So candidates fall into two groups, and the second is a different project from
the first:

- **C sources AOK can compile itself.** bash, zsh, OpenSSH and nextvi are
  already here. Cost is the porting work the gate enumerates
  (`tools/check-native-libc.py --report <objects>`), which is finite and
  visible up front.
- **Anything built by a foreign toolchain** -- Rust, Go, or a vendored build
  system we do not drive. Their `open`/`read`/`write` resolve to the host's at
  link time and no `#define` reaches them. Making one work needs link-time
  symbol interposition (rename the libc imports in those objects to the
  `nlibc_` entry points), which AOK does not have today. **Building that
  mechanism is the prerequisite for the whole second group**, and is worth
  costing once rather than per program.

### helix

Requested 2026-08-20. Modal editor, Rust, MPL-2.0 (confirm before any work --
the licence matters here the way GPLv3 does for bash, which is why
`-Dnative_bash` exists at all).

Second group, so it is behind the interposition question above. Beyond that:

- Rust std does its own syscalls, and helix does file I/O throughout -- there
  is no pure/impure split to exploit the way zlib's deflate/inflate allowed.
- LSP servers and formatters are spawned processes. `fork()` is ENOSYS for a
  native program, but exec/spawn works (see [[native-exec-standin]]), so this
  is probably not the blocker it first looks like.
- Tree-sitter grammars are built as loadable objects by default; a static
  grammar build would be needed.
- Size is tens of MB with grammars, against a binary that ships in an app.

AOK already has nextvi and micro native, so this is the "modern editor" slot
rather than a gap. **Next step** is the interposition prototype, not helix
itself -- pick the smallest Rust program that does one `open` and see whether
its objects can be made to call `nlibc_open`.

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
| -- | btop shows nothing in its disk, net and io sections | reported 2026-08-19; **fixed** -- see *Closed during the 550 cycle* |

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
