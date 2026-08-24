# Release Notes Since `builds/iSH-AOK_549`

167 commits. Where 549 was about making native programs *believe* they were
inside the guest, this release is mostly about the things you actually touch:
a file browser you can reach without leaving the terminal, an editor, helix,
themes worth using, and a run of fixes for bugs that took the app down or made
it look broken.

## Highlights

**A file browser, in the terminal.** Tap the folder key on the keyboard bar — or
press Cmd-B — and a sheet slides up over the terminal. It opens **in the
directory your shell is already standing in**, and its verbs type rather than
open: tapping a file puts its path at your cursor, `cd Here` runs a real `cd`.
Everything it types is shell-quoted first, so a file called `my report
(final).txt` arrives as one argument and a file whose name contains `&&` cannot
run anything. New Folder, Rename, Duplicate, Delete and Get Info are a
long-press away. See `/AOK/docs/file-browser.md`.

**helix**, as `/AOK/native/hx`. A modal editor with syntax highlighting and
multiple selections, compiled into the app — so it costs the same under an i386
root as under an arm64 one. Getting there meant running **Rust** natively, by
rewriting its libc imports onto the shim rather than the `#define` redirection
that only reaches AOK's own translation units.

**motepad**, the terminal half of Workspace's editor, deliberately modeless: you
type and the characters appear. `/AOK/docs/motepad.md`.

**Ten new terminal themes** — Catppuccin, Dracula, Everforest, Gruvbox,
Kanagawa, Nord, One Dark, Rosé Pine, Tokyo Night and Tokyo Night Storm — taken
from each project's published values rather than approximated. Fifteen built-ins
now. `/AOK/docs/themes.md`.

**`/AOK/persist/bin`, `lib` and `etc`**, first on `PATH`: somewhere to keep
programs that outlive the root filesystem they were built for. A `ws-*` launcher
is generated there for every Workspace applet, so the guest can open one by
running a command.

**The Linux AIO family** (`io_setup`, `io_submit`, `io_getevents` and the rest)
is implemented on all four guest ABIs.

## Crashes and hangs that reached users

*The System Console never gave you a prompt on Devuan.* A hangup belongs to the
descriptors that were open at the time, not to the terminal itself — AOK modelled
it as one sticky flag, so after bootlogd released the console every later `open`
still failed and `init` gave up respawning getty. Fixed, with the reproduction
narrowed to booting the guest's own init under a pty.

*MariaDB never finished installing.* An absolute path ignores `dirfd`; AOK was
rejecting the combination instead. The server now answers queries on device.

*The emulator could wedge.* A lost wakeup on Darwin's rwlock could leave a
thread asleep on a lock nobody held.

*eudev refused to start*, having decided AOK was a container — because nothing
was running as pid 2. There is a synthetic `kthreadd` now.

*Copying a file into `/AOK/persist` could freeze both file managers.* A
cross-backend copy read the entire file into memory on the one queue every
filesystem operation shares. It streams in chunks now, and bulk transfers no
longer sit in front of directory listings.

*Typing could arrive out of order* — `mkdir` became `kmdir` — because the
keyboard had two delivery routes that were not sequenced against each other.

*`apt search maria` wedged the whole app.* The pager took the blame and was
removed from `PATH` for it, but the trigger was ours: native dispatch was not
applying close-on-exec, so apt's four-byte exec-status handshake never saw EOF
and both sides waited forever. Fixed in `kernel/exec.c`; SmallCLUE's `less` and
`more` are back on `PATH`, and the pager now streams — first screen as soon as
it exists — because reading a whole input before drawing a line was a hazard
whatever the trigger.

*A dangling pointer wrote a byte into dead stack frames for the life of a
task.* `kernel/futex.c` publishes the address of one of its own stack locals in
`waiting_interrupt_flag`, and `wait_for`'s first early return skipped the only
code that clears it — so from then on another thread stored `true` through a
pointer into a frame that had died. It happens several times in every run of
`pread_stack_thread_race`, which is the test that had been SIGSEGVing about one
run in eight.

## Elsewhere

- **The Files app integration is disabled on a Mac**, deliberately: the classic
  `NSFileProviderExtension` API is unavailable on macOS, and the framework was
  killing the extension before any AOK code ran. The app says why instead.
- **`curl`, `wget` and `md <url>`** work, over an NSURLSession-backed libcurl.
- **Real `tar`, `gzip`, `gunzip` and `zcat`**, over guest-backed gzip streams.
- **`ktop`** reports a guest architecture for kernel threads instead of `?`,
  and the **prebuilt binaries actually contain that fix**. The job that
  cross-compiles and commits them had failed on every run since before build
  547 — it pinned a setup action that resolves versions against Zig's
  dev-builds path, and 0.16.0 is a release — so the shipped binary had been
  quietly frozen for four releases while `ktop.c` moved on.
- **`kill()` is process-directed**, delivered to a thread that can take it.
- Directory listings **stat against the open directory** rather than re-walking
  every path from `/`, which collapses the cost from O(entries × depth).

## Documentation

Three pages for features that had none: `themes.md`, `md.md` and `proc-ish.md`.
Nineteen pages now.

All three were then rewritten, because a verification pass caught seven wrong
claims in them — including two that reported a capability as absent when it is
not. `/proc/ish/defaults` is **writable by root**, and a write takes effect live
through the same validator Settings uses; `md`'s `-l` flag is in its usage text,
not missing from it. A page that says a thing cannot be done is the kind of
error nobody reports, because the reader simply believes it.

A full pre-release audit found and fixed a page that would have cost people
their symlinks: `native-links.sh --remove` had been widened to own anything
under `/AOK/native`, while the docs still promised a hand-made link to
`/AOK/native/bash` was safe.

The Korean and Chinese READMEs are brought back into line with the English one —
both had been declaring themselves a stale translation, and both claimed the app
bundles rootfs images for all four architectures when only arm64 is bundled.

## Validation

- tier0: **114 passed, 0 failed** on both i386 and x86_64
- The guest regression suite run across **all four architectures** on a 2nd-generation 12.9-inch iPad Pro — arm64, i686, x86_64 and riscv64 — with no failure that was not an artifact of running mount tests inside a chroot. Those now SKIP rather than fail.
- meson unit and e2e suites, and the host-side gates: native-libc purity, bash TLS, shell quoting, applet audit
- Xcode Debug and Release for arm64 device
- GitHub CI green — which it had not been all cycle, on two counts: an unguarded BSD-only header broke every Linux build, and no workflow installed the Rust iOS target, so **tagging this release would have failed to produce an IPA**

## Known gaps

- The JVM does not start on aarch64 — HotSpot aborts during startup with "Field
  too big for insn" ([#542](https://github.com/emkey1/ish-AOK/issues/542)), which
  blocks every JVM-based tool. Under investigation.

Three gaps that stood at the start of this cycle are closed rather than carried:
`pread_stack_thread_race`, the excluded pager, and ptraceomatic's divergence at
instruction 4175 — which turned out to be the tool rather than the emulator.

## Commit Range

`builds/iSH-AOK_549..builds/iSH-AOK_550` — 167 commits.
