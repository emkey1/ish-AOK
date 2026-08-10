iSH-AOK 547
36 commits. Mostly crash fixes, and three of the four families were found in the
field rather than in testing.

systemd-networkd crash-looped on boot -- Darwin has flagless network interfaces
and Linux does not. `ifconfig stf0`, the 6to4 tunnel present on macOS and iOS
alike, prints literally `flags=0<>`, and we forwarded that zero into the netlink
link dump. networkd's `link_update_flags()` early-returns when the incoming flags
and operstate both match a freshly zeroed Link, so it never reached
`link_update_operstate()`, left `carrier_state` in a NULL hole of its lookup
table, and aborted on an assertion. `Restart=` did the rest.

Two terminal sessions were enough to abort the emulator -- every session
inherited init's UTS namespace pointer without taking a reference, while exit
released it like any other, so the second session to exit drove the refcount to
zero and called `free()` on a static object.

Both 0xdead10cc families closed -- iOS kills a process suspended while holding a
SQLite lock, and we managed it two different ways. The File Provider extension
was closing its database at teardown, where `sqlite3_close()`'s WAL checkpoint
ran straight into the suspension deadline; it now closes on an idle timer, taking
65 microseconds while the process is alive and untimed. The main app was being
suspended mid-transaction, which needed the opposite fix: a gate that drains
in-flight transactions before suspension, plus assertions around the boot mount
phase. Between them, these were 100% of build 546's crash reports and 41 more
across 516 to 545.

Go programs randomly lost children -- getppid, procfs and taskstats reported the
parent THREAD id where Linux reports the parent process. The two are identical
whenever the forking task leads its thread group, which is every shell and every
single-threaded program, so it hid for years. Go forks on whatever thread the
runtime scheduled, so a child with Pdeathsig set saw a "parent already died"
mismatch and killed itself before reaching execve. (GH #523, yay on Arch ARM64.)

Sockets are killable again -- tasks blocked in recv, send, and plain read/write
on a socket survived SIGKILL indefinitely, the same defect 546 fixed for accept
alone.

TLS 1.3 works -- the crypto provider was registering a ChaCha20-Poly1305
implementation that broke every session it touched. AES-256-GCM is hardware
accelerated in its place.

amd64 got block chaining, which immediately exposed that entering a chained block
without publishing the guest rip made every static amd64 binary SIGILL at a
mid-instruction address.

Two bugs the release testing caught, both of which would have shipped:
- The on-device regression suite had been unable to start since `4102fc1d`. Two
  test sources were missing from the manifest that embeds /AOK/tests, and the
  runner aborts on the first missing file, so it did not skip a test, it stopped
  the entire run. Any device validation recorded against 546 either predates that
  commit or never executed. The build now fails rather than shipping a suite that
  cannot run.
- The new suspension gate froze the guest filesystem whenever iOS backgrounded
  the app without actually suspending it. sshd would accept a connection and then
  hang before reaching a shell, because fork, exec and PAM each need the
  filesystem. Now bounded.

Validated: all four guest architectures, zero failures (i386 110, amd64 110,
arm64 108, riscv64 101), plus the on-device native suite (103) and the
four-architecture concurrent chroot pass (111/109/111/102) -- 433 tests
contending on one shared heap inside a single emulator, which is the
configuration that found the mm_copy heap corruption back in 538.

Known: the real-Linux oracle runs (netlink_route, socket_kill,
vfork_fatal_signal) are outstanding. All pass under emulation on multiple guest
ABIs; what is missing is the cross-check against a real kernel. The networkd fix
is verified against an image that reproduces the reported crash boot for boot,
but has not run on the reporting device.
