# Release Notes Since `builds/iSH-AOK_546`

Thirty-six commits, and this one is about staying alive. Four separate crash
families are closed here, three of them found in the field rather than in
testing: a `systemd-networkd` crash-loop that a user photographed mid-boot, an
over-release that free()d a static object after the second terminal session of
every run, and both halves of the `0xdead10cc` suspension kills that account for
every crash report build 546 produced. Underneath sits the rest of the
unkillable-socket-wait class opened at 546, amd64 block chaining, and hardware
AES-256-GCM.

The release also found something uncomfortable about itself. Running the
on-device regression suite as part of validation revealed that it had been
unable to start at all since `4102fc1d`, because a test source was missing from
the manifest that embeds `/AOK/tests` and the runner aborts on the first missing
file. That is fixed twice over, and the build now fails rather than shipping a
suite that cannot run.

## Highlights

**systemd-networkd no longer crash-loops on boot.** Darwin has flagless network
interfaces and Linux does not: `ifconfig stf0`, the 6to4 tunnel present on macOS
and iOS alike, prints literally `flags=0<>`, and iSH-AOK forwarded that zero
into the netlink link dump. No Linux netdev has an empty flag word, and
networkd relies on it -- `link_update_flags()` early-returns when the incoming
flags and operstate both match what a freshly zeroed `Link` already holds, so it
never reached `link_update_operstate()`, left `carrier_state` in a NULL hole of
its lookup table, and aborted on an assertion. `Restart=` did the rest. Fixed in
the shared flag mapper so the dump and `SIOCGIFFLAGS` stay consistent
(`0c45960c`).

**Two terminal sessions used to be enough to abort the emulator.** Every new
session inherited init's UTS namespace pointer without taking a reference, while
`do_exit` released it like any other, so the second session to exit drove the
refcount to zero and called `free()` on a static object. Nothing else notices a
missing retain, because it is the release side that crashes (`f8253b10`).

**Both `0xdead10cc` families are closed.** iOS kills a process suspended while
holding a file or SQLite lock, and iSH-AOK was doing it in two unrelated ways.
The File Provider extension was closing its database at teardown, where
`sqlite3_close()`'s WAL checkpoint ran straight into the suspension deadline;
it now closes on an idle timer instead, while the process is alive and untimed
(`e5d0f244`). The main app was being suspended mid-transaction, which is a
different problem needing a different answer: a quiesce gate that gets the
filesystem to a lock-free state, plus assertions around the boot mount phase
(`38226660`, bounded in `35d65f26`).

**Go programs stopped losing children at random.** Every user-visible parent pid
was the parent *thread* id where Linux reports the parent *process*. The two
agree whenever the forking task leads its thread group, which is every shell and
every single-threaded program, so it hid for years. Go runs `fork` on whatever
thread the runtime scheduled, so a child with `Pdeathsig` set would see a
"parent already died" mismatch and kill itself before reaching `execve`
(`cf88548d`, GH #523).

**Sockets are killable again, and TLS 1.3 works.** `745b8f80` finishes the class
`26d5cc9a` opened: tasks blocked in `recv`, `send`, and plain `read`/`write` on a
socket survived `SIGKILL` indefinitely, for the same reason `accept` did. And
the crypto provider was registering a ChaCha20-Poly1305 implementation that broke
every TLS 1.3 session it touched (`c2587dfd`).

## User-Facing Changes

### Emulation

- amd64 blocks now chain, with the guest `rip` published on chained entry
  (`4c279c9c`). This engine has helper gadgets that re-decode their instruction
  out of guest memory, so entering a chained block without a correct `rip` made
  every static amd64 binary die with `SIGILL` at a mid-instruction address.
- AES-256-GCM is accelerated through the host (`f644aafa`) and wired into the
  OpenSSL provider (`94be80d6`).
- The vfork parent wait is now killable, and the structure it waits on is
  refcounted so the child cannot be left pointing into a dead stack frame
  (`1c3266de`, GH #506).
- `getppid(2)`, `/proc/PID/stat`, `/proc/PID/status` and taskstats all report the
  parent process rather than the parent thread (`cf88548d`).

### Filesystem and kernel

- Tasks blocked in `recv`/`send`/`read`/`write` on a socket now die promptly on a
  fatal signal (`745b8f80`), completing the work `26d5cc9a` and `9e192bb4`
  started for `accept`. `02b4c273` corrects the notify-pipe claim and creates the
  pipe lazily.
- The UTS namespace inherited by a new init child is retained (`f8253b10`).
- Empty-string tests no longer route through `strcmp` (`08c8ea83`).

### App

- Terminal fonts are chosen by measuring them rather than trusting the monospace
  trait (`5328d407`), which is also why CaskaydiaCove Nerd Font was going missing
  (`621ba0c3`).
- The bookmark button announces its state to VoiceOver correctly: `bfe64799`
  added the selected trait and value, and `b08b3480` removed the half that
  duplicates the trait while keeping the half that is the only signal when the
  page is not bookmarked.
- `uname -v` and `/AOK/VERSION` report the build timestamp, in UTC, rather than
  only a version number that does not move between builds (`c0e8d72b`,
  `e1597912`). This matters more than it sounds: the version number cannot tell
  you which code a device is running.
- Device model tables gained the entries they were missing, including the iPad
  5th generation, which was reporting its raw `iPad6,12` identifier and falling
  through the core-topology fallback that an A9 actually needs (`1668df18`).
- The synthesized `/AOK` filesystem reports real timestamps (`201f7e86`).

## Validation

Four architectures on the CLI, the native suite on device, and the full
concurrent-chroot pass, all with zero failures:

| run | PASS | FAIL |
|---|---|---|
| CLI i386 / amd64 / aarch64 / riscv64 | 110 / 110 / 108 / 101 | 0 |
| Device native aarch64 (iPad Pro M4) | 103 | 0 (15 privilege skips) |
| Device concurrent 4-arch chroot | 111 / 109 / 111 / 102 | 0 |

The concurrent pass runs i386, aarch64, x86_64 and riscv64 chrooted into
separate roots simultaneously inside one booted emulator, all as root: 433 tests
contending on a single shared heap and lock set. The app survived with no crash
reports. That configuration is the one that surfaced the `mm_copy` heap
corruption during 538, so a clean run there is worth more than the per-arch
totals.

Individually verified on device via Console: the File Provider idle close
(`idle close: dropping 9 mount(s) after 3.0s idle`, completing in 65
microseconds) and the main-app quiesce (`quiesced for suspension: drained=1
straggling=0`, 26.6 seconds after backgrounding).

## Known Issues

- The real-Linux oracle runs are outstanding: `netlink_route` for the networkd
  fix, plus `socket_kill` and `vfork_fatal_signal`, all unreachable during this
  cycle. Every one of them passes under emulation on multiple guest ABIs; what
  is missing is the cross-check against a real kernel.
- The networkd fix is not device-verified. It is verified against the amd64 Arch
  image that reproduces the reported crash exactly, boot for boot, but the
  reporting device has not run it.
- Every link stays in `LINK_STATE_PENDING` under iSH-AOK, because
  `sd_device_new_from_ifindex()` fails: there is no `/sys/class/net/<ifname>`.
  Tolerated by networkd, but it means link configuration never proceeds.
- The main app's `0xdead10cc` fix is proven at the mechanism level, not by the
  absence of the crash. Those 41 reports accumulated across 30 builds, so the
  field signal is far too slow to confirm within a release cycle.
- A fakefs root whose metadata disagrees with the host about an entry's type
  does not self-heal, and every `fsopen`-based mount on it fails permanently.
  One test fixture was found in that state, repaired by hand.

## Maintainer Notes

- **The on-device suite was dead and nobody knew.** `setup-regressions.sh` calls
  `need_file` for each source it requires and exits on the first one missing, so
  an unlisted test does not skip that test, it stops the entire run before a line
  of it executes. Two files were missing from `fs/aok-tests.manifest`:
  `inaddr_any_iface.c` since `4102fc1d` (`25963682`), and `x86/port_io_gpf.c`
  (`3487cba8`). Any device validation recorded against 546 either predates
  `4102fc1d` or did not actually run. `tools/gen-aokfs.py` now compares the
  manifest against the runner at build time and fails with the list, because the
  two are edited in different places and otherwise only meet on a device.
- The publishing workflows are guarded on repository name (`499dc531`,
  `c68c0ac0`). A checkout whose origin had been repointed at an unrelated project
  pushed `working` there, and that repository ran this repository's workflow and
  published a 40 MB iSH-AOK IPA under someone else's name. Matching on name
  rather than owner keeps forks working.
- The crypto accelerator is usually a loss, and the numbers in `a770adaa` and
  `02d63578` are from optimized builds on real devices. An `-O0` build does not
  merely run slower, it inverts this particular conclusion.

## Commit Range

```
35d65f26 app: bound how long the fakefs suspension gate may stay engaged
3487cba8 /AOK: embed x86/port_io_gpf.c, and fail the build when the manifest lags
25963682 /AOK: embed inaddr_any_iface.c, which no on-device suite could run without
38226660 app, fs: don't hold a fakefs SQLite lock across suspension
1668df18 hostinfo: fill in the device models that were missing from the tables
e5d0f244 fileprovider: close the fakefs databases when idle, not during suspension
f8253b10 kernel: retain the uts namespace a new init child inherits
b08b3480 app: drop the bookmark button's redundant "Bookmarked" value, keep the other
08c8ea83 fs, kernel, tools: test for an empty string directly, not via strcmp (#551)
c68c0ac0 ci: match the publishing guards on repository name, so forks still build
499dc531 ci: only let the publishing workflows run in this repository
621ba0c3 app: correct why CaskaydiaCove Nerd Font goes missing
5328d407 app: pick terminal fonts by measuring them, not by the monospace trait
0c45960c net: never publish a link with no flags, which SIGABRTs systemd-networkd
4c279c9c jit: chain amd64 blocks, and publish rip on chained entry
4102fc1d tests: probe that a wildcard bind really covers every host address
745b8f80 fs: close the rest of the unkillable-socket-wait class, not just accept
a770adaa docs: crypto accelerator numbers from optimized builds on both devices
94be80d6 crypto: accelerate AES-256-GCM in the OpenSSL provider
02b4c273 fs: correct the accept notify-pipe claim, and create the pipe lazily
9e192bb4 fs: give the accept(2) wait a non-lossy wake channel, not just the SIGUSR1 poke
201f7e86 /AOK: give the synthesized filesystem real timestamps
02d63578 docs: the crypto accelerator is usually a loss, say so
e1597912 uname: stamp the build in UTC, not the device's local timezone
c0e8d72b uname, /AOK/VERSION: report the build timestamp, not just the version number
790376f2 tests: correct accept_kill's validation record
afeac3c6 fs: bundle vfork_fatal_signal.c and getppid_thread.c into /AOK/tests
f644aafa kernel/ish_accel: host-native AES-256-GCM (alg 2)
26d5cc9a fs: make the accept(2) wait interruptible, so a blocked listener is killable
c2587dfd crypto: stop registering ChaCha20-Poly1305, it breaks every TLS 1.3 session
f3b9e6d4 opt/AOK: ship the crypto accelerator's setup at /AOK/tools/crypto
f54b6bc5 tests: scale getppid_thread's procfs-read window with ISH_TEST_WATCHDOG_SCALE
cf88548d kernel, fs: report the parent PROCESS, not the parent thread (GH #523)
bfe64799 🎨 Palette: [UX improvement] Improve bookmark button accessibility (#548)
1c3266de kernel: fix the vfork parent wait's lifetime and signal handling (GH #506)
fb26d22d docs, tests: call the product iSH-AOK, not iSH
```
