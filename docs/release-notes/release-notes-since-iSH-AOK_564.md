# Release Notes Since `builds/iSH-AOK_563`

28 commits. A short cycle, mostly fixes from what testers sent in. Five
kinds of crash on 563 are fixed, the worst of them taking the whole app down when
you opened a file under `/proc` from the file manager. The external display now
works on iOS 27 in the simulator, though it still has not been seen on real
hardware. Lookups on a `mount -t ios` folder are hundreds of times faster.

## Before you update

**Saved sessions from 563 will not restore.** An image is tied to the build
that saved it. The guest boots fresh, and the resume picker marks such an
image "can't be restored".

## Crash fixes

All five were reported by TestFlight on 563.

- **Opening a file under `/proc` from the app ended the app.** Tapping one with
  no extension in the file manager did it, as did opening one in a viewer, in
  MotePad or with LLM Chat's read tool. One iPhone crashed this way three times
  in six minutes. The app read such files in a way `/proc` does not support, and
  it now falls back as a guest's `read` does.
- **Leaving and coming back during a long save could crash the app.** Each trip
  to the background started another save, the first one's end woke the guest
  under the second, and the second then walked processes as they exited. Only
  one save or restore runs at a time now. A second automatic save is skipped, and
  **Save Session** and the other buttons wait their turn. The two saves also wrote
  the same temporary file, so they could have spoiled an image without crashing.
- **A read of `/dev/location` could crash the app** when it raced a location
  update, seen 94 minutes into a session.
- **A guest's unknown Vulkan format ended the whole app.** Now only that guest
  process dies, with `SIGABRT`: Venus does not wait for a render pass to be
  created, so the guest's driver hears of the failure afterwards and aborts.
- **iOS could kill the File Provider while it closed its database when idle.**
  It now stays awake through the close. iOS's suspension could not be forced in
  the simulator, so the paths for running out of time have not been run.

## External display on iOS 27 (#540)

563 gave only a mirror on iOS 27, as an iPhone 15 showed. iOS 27 stopped creating
the external scene from the app's `Info.plist` for apps built with its SDK. An app
now has to ask for it from a view controller, and 564 does. A Workspace terminal
window also reaches the display now, which it never did before.

**This has run only in the iOS 27 simulator**, and the Wayland mirror has not run
there either. If you try it on a real display, please say what you saw on
[#540](https://github.com/emkey1/ish-AOK/issues/540), working or not.

## Other fixes

- **`mount -t ios` folders: lookups no longer coordinate with other apps.** `rclone`
  checking 10,000 photos on an SSD took 3 minutes on an iPhone against 1-2 seconds
  on a Mac. In the simulator, `find` over 10,000 files went from 168 s to 0.5 s.
  Opening a file and changing anything still coordinate.
  `/AOK/docs/files-app-integration.md` now describes these mounts.
- **GNU `touch` works on `mount -t ios` and FUSE mounts.** It said "Not a directory"
  and left the times alone.
- **`btop`'s boxes and columns line up.** No monospace font on iOS has braille, so
  it came from a fallback font at that font's own width. Such characters now
  stay one cell wide.
- **`/proc/stat`'s CPUs add up to its total.** `stress-ng --cpu 6` on a 6-core iPhone
  showed two CPUs at 0% in `btop`, which was reported as the performance cores
  never starting. They were running. A guest within `nproc` still leaves the CPUs
  iOS keeps for its UI reading idle.
- **Xfce on Alpine opens no error dialogs at login, and notifications work.** Alpine
  builds `xfce4-notifyd` for X11 alone, so the session runs it under Xwayland, which
  stays up at about 30 MB.
- **`suspend.sh` says when a save was refused.** `--save FILE` printed "saved to
  FILE" for a save that wrote nothing, and a refused suspend ended silently. Both
  now print the reason and exit 1. A plain `echo save ...` to `/proc/ish/checkpoint`
  still succeeds, since the save runs just after the write; check `last_path`.
- **x87: `FCOMI`, `FCOMIP`, `FUCOMI` and `FUCOMIP` keep C1**, as an AMD Ryzen does,
  where they used to clear it as Intel's manual says.
- **VoiceOver skips the breadcrumb chevrons** in the file browsers (#667).
- **The `dev-latest` release** is updated in place, so a publish that fails part way
  still leaves a build to download.

## Testing

The full regression suite on the five release roots — Alpine 3.24 i386, amd64,
arm64 and riscv64, and Devuan 6 arm64 with glibc — on this build:

| root | M5 Mac (command line) | A10X iPad (the app) |
|---|---|---|
| i386 | 458 passed | 461 passed |
| amd64 | 478 passed | 483 passed |
| arm64 (musl) | 395 passed | 400 passed |
| riscv64 | 387 passed | 392 passed |
| arm64 (Devuan, glibc) | 395 passed | 399 passed |

No failures on the iPad. On the Mac, `inaddr_any_iface` failed once on i386, with the
Tailscale address and the other legs running beside it, and passed when run alone.
Everything that passed on the Mac also passed on the iPad, and the iPad's extra passes
are tests the Mac skips.

The iPad is a 2017 12.9" iPad Pro on iPadOS 17.7, and each root was booted as the running
root, not reached through a chroot. Its i386 and amd64 roots are Alpine 3.23.3, and its
riscv64 root is Devuan 6 with glibc. Its Devuan arm64 leg ran as the default user, with
the 17 tests that skipped there then run as root.

**No device on iOS 26 or 27 ran the suite for this build.** On iOS 27 it has run only in
the simulator, in the checks described below.

The ppc64le root was not run for this build. The e2e suite passed 7 of 7 from a
freshly built test filesystem.

The new `checkpoint_overlap.sh` crashed the old code and made 72 overlapping saves on
the new, each image restoring with its memory intact. The app itself was bounced
between background and foreground during a 1.5 GB save in the iOS 27 simulator, and
logged the second save being skipped. A real device has not seen that crash fixed: that
the iPad's crash on 563 was two saves overlapping is inferred from its log.

## Known issues

- **The external display is untested on hardware** (see above).
- **iOS can end the app seconds into a launch** (0xdead10cc, seen once on an iPad on
  iOS 27.2), when it suspends the app while a guest's file change is being committed to
  the root's database.
- **Run `suspend.sh` by its path.** `sh /AOK/tools/suspend.sh` on a root with the native
  links uses native dash, which a resume starts again from the top, so the session
  suspends again.
- **Sandboxes cannot reach the network** through a veth: there is no forwarding or
  bridge yet.
- **ssh sessions still drop when the device locks**
  ([#631](https://github.com/emkey1/ish-AOK/issues/631)): iOS takes a suspended app's
  connections.
- **A native program's own threads share one task** and are not stopped when the
  program ends.
- **Xfce draws no desktop backdrop** on some setups: icons on black.
- **On distribution roots, `who` misses the app terminal's own login.**
- **ppc64le is experimental**: no speed work yet, and no POWER9 instructions.
