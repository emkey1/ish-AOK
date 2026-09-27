# Release Notes Since `builds/iSH-AOK_533`

These notes summarize changes from `builds/iSH-AOK_533` intended for `builds/iSH-AOK_534`.

This is a focused release (13 commits). The headline is **iOS 27 support**: iSH-AOK now builds with the **Xcode 27 beta** and runs on the **iOS 27 beta**, and the minimum deployment target rises to **iOS 15** (previously iOS 12 — iOS 12–14 are no longer supported). It also lands a new **Workspace Music player**, fixes several crashes that struck on iOS 26/27, and makes `lsof` / `/proc/<pid>/fd` report real file-descriptor types.

## Highlights

- **Runs on the iOS 27 beta.** Built with the Xcode 27 beta; the instant on-boot crash on iOS 26/27 (any `/proc/meminfo` read) and an About-screen crash are fixed. Minimum iOS is now 15.
- **New Workspace "Music" player applet.** A native audio/music player in the Workspace — playlists, a now-playing scrubber, shuffle/repeat, and background playback with lock-screen / Control Center controls. Plays the system-native formats (MP3, AAC/M4A, ALAC, WAV, AIFF, CAF, FLAC) out of the box, with **Ogg Vorbis and Opus** added via vendored open-source decoders (no private Apple APIs).
- **`lsof` and `/proc/<pid>/fd` show real fd types.** Sockets, pipes, and eventfd/epoll/signalfd/timerfd/inotify descriptors are identified correctly instead of all appearing as `anon_inode:[unknown]`.
- **Crash fixes** for an exec-time `SIGBUS`, a byte-range-lock NULL dereference, and the `/proc/meminfo` boot abort.

## User-Facing Changes

### iOS Support

- iSH-AOK now **builds with the Xcode 27 beta and runs on the iOS 27 beta**. The **minimum deployment target is raised to iOS 15** (was iOS 12); **iOS 12–14 are no longer supported**.
- **Fixed: instant crash on boot (iOS 26/27).** Any guest read of `/proc/meminfo` (busybox `top`/`free`, init scripts) aborted the app, because `get_mem_usage()` asserted that the host memory-stat calls succeed and those calls can now fail on iOS 26/27. This was the build-533 "insta-crash," which appeared once a Devuan root (bundling busybox `top`/`free`) was installed. The app now degrades gracefully (falls back to `sysctl hw.memsize`).
- **Fixed: About / Settings screen crash (iOS 27).** iOS 27's UIKit bounds-checks the static storyboard table sections, and the dynamically-appended "LLM Client" section tripped an out-of-bounds during layout.
- **Fixed: Workspace menus unresponsive (iOS 27).** Menu items that open a submenu or sheet did nothing, because iOS 27 drops a presentation that races the tapped menu's dismissal; every such item now defers correctly.

### Workspace — Music Player

A new **Music** applet, opened from the Workspace root/Utilities menu (Workspace group), plays audio from the guest filesystem:

- **Library.** Defaults to `/AOK/persist/music` (a real-fs mount, so it survives root switches), with **Add from Path…** to queue a single track or a whole folder from anywhere in the guest. Files inside a root's fakefs are read through the emulated VFS, so they play even though they aren't host files.
- **Playback.** Now-playing title/artist, a draggable scrubber, play/pause, previous/next, shuffle, and repeat (off / all / one); volume persists. Playback is independent of the window — closing the Music window does not stop the music.
- **Background + lock screen.** Audio keeps playing when iSH-AOK is backgrounded or the device is locked, with play/pause/skip and scrubbing from the lock screen and Control Center (new `audio` background mode).
- **Playlists.** Save the current queue as a named playlist and reload it later (stored as JSON under `/AOK/persist/playlists`).
- **Formats.** MP3, AAC/M4A, ALAC, WAV, AIFF, CAF, and FLAC decode natively via `AVAudioFile`; `.ogg` (Vorbis) and `.opus` decode via the vendored libvorbis/libopus.

### Workspace — Terminal Windows

- The **× close button now works** on Workspace terminal windows — the title-bar drag recognizer was cancelling the button's tap on the slightest movement.
- Typing **`exit` now closes** the embedded terminal window instead of respawning the login shell.

### Diagnostics — `lsof` and `/proc`

- `/proc/<pid>/fd` entries — and therefore `lsof` — now report **real descriptor types**: sockets as `socket:[inode]`, pipes as `pipe:[inode]`, and eventfd/epoll/signalfd/timerfd/inotify as `anon_inode:[<class>]`, each with a unique inode. Previously every anonymous fd showed as an unstattable `anon_inode:[unknown]`, so (for example) a daemon's listening socket was invisible to `lsof`.

### Stability

- **execve `SIGBUS` fix.** A writable ELF segment whose file contents end partway through the last 16 KB host page could `SIGBUS` during `execve` on iOS (APFS "cluster_pagein past EOF"); the straddling tail is now mapped anonymously.
- **fcntl byte-range lock crash fix.** `file_lock_from_flock()` could dereference a NULL function pointer (seen on builds 526–530); the `SEEK_CUR`/`SEEK_END` degenerate paths now return `ESPIPE` instead of crashing.
- **Log-spam fix.** The "Invalid UIScreen coordinate space conversion" spam from the keyboard-frame handler (with multi-scene or second-window terminals) is gone.

## Known Issues

- **`signal_poll` can fail under heavy concurrent load** (long-standing; not new in 534). With many emulated processes running at once, a signal meant to interrupt a blocked `poll()`/`select()` can arrive *after* the call's own timeout — so it returns `0` instead of `EINTR` — because iSH multiplexes guest signal delivery and inter-CPU TLB-invalidation pokes over the same host `SIGUSR1`, which congests under load. Single-process / sequential runs (how the suite actually runs) pass reliably (50/50 here, both engines); the failure only reproduces under deliberate concurrency (~3 of 4 simultaneous instances).
- The new Workspace Music player has had limited on-device runtime testing — the streaming buffer pipeline, scrubbing, and lock-screen controls were exercised by the maintainer but not across all formats and devices.
- Most emulator-level fixes were validated on an arm64 host (bit-exact vs the Rosetta x86 / "mint" real-Intel oracles per their commit messages) and need this app build to reach the device.

## Maintainer Notes

- **iOS 27 / Xcode 27.** Building against an older SDK crashes pre-`main` in libxpc's `_xpc_init_pid_domain` on the iOS 27 beta (Apple FB19282108, "unrecognized selector `-[OS_dispatch_mach_msg _setContext:]`", zero app frames) — the app must be built with the Xcode beta that ships the device's iOS SDK. The deployment target is now **iOS 15.0**, set authoritatively in `app/Project.xcconfig`; `tools/wire_audio_player.rb` no longer strips the `xcodeproj`-gem-inserted `IPHONEOS_DEPLOYMENT_TARGET = 15.0` lines (its old iOS-12 cleanup silently reverted the bump on every run).
- **App Store packaging — vendored liblzma.** This finishes the on-device xz-import feature begun in 533. iOS's system `liblzma` is private, so linking `liblzma.tbd` makes App Store validation reject the app for referencing non-public `_lzma_*` symbols. It is replaced with a statically-linked **liblzma 5.8.3** built from the xz `v5.8.3` git tag under `deps/liblzma-static/` (decode-only at runtime, single-threaded, `HAVE_SMALL`). **Encoders are compiled even though the app never encodes** — libarchive's xz/7z/xar *writer* objects reference `lzma_*_encoder` entry points at link time under `HAVE_LZMA_H` (which the xz reader requires), so a decode-only build fails to link. Rebuild via `deps/liblzma-static/build-liblzma-apple.sh`; `libbz2`/`libz` stay system links (public on iOS).
- **Vendored audio codecs (App Store-safe), mirroring liblzma.** Ogg/Opus use the open-source Xiph decoders (libogg/libvorbis/libopus/opusfile), statically linked via `deps/audiocodecs-static/audiocodecs.xcframework` — **no Apple private interfaces** (the same reasoning as the vendored liblzma). One-time build: `deps/audiocodecs-static/build-audiocodecs-apple.sh` downloads the Xiph release tarballs and cross-compiles `ios-arm64` + `ios-arm64_x86_64-simulator`; commit the resulting `audiocodecs.xcframework` + `Headers/` like liblzma.
- **Graceful degradation.** The Ogg/Opus decoder backends are gated by `__has_include`, so the app compiles and plays every native format **before** the codec xcframework exists; `.ogg`/`.opus` are simply hidden from the picker until it's linked. After building the xcframework, re-run `ruby tools/wire_audio_player.rb` to link it.
- **Audio layout.** New `app/` files (`AudioPlayerEngine`, `AudioPCMDecoder{,_AVF,_Vorbis,_Opus}`, `AudioLibrary`) compile in `libiSH-AOKApp`; the applet view controller lives in `WorkspaceViewController.m`. AVFoundation + MediaPlayer link in `iSH-AOK` / `iSH-AOK+Linux`. The pbxproj wiring (sources, frameworks, codec header search path) is done idempotently by `tools/wire_audio_player.rb` via the `xcodeproj` gem.
- **New `/AOK/tests` regressions.** `mount_flags.c` and `clone_error_cleanup.c` are run by `setup-regressions.sh`'s `all_tests` but had been missing from `fs/aok-tests.manifest`, so they were never embedded into `/AOK/tests` (on-device `need_file` failed); both are now listed.
- **lsof fix internals.** Adhoc fds (`fs/adhoc.c`) now get a unique inode and render their type from `stat.mode` (socket → `socket:`, pipe → `pipe:`) or a new `fd_ops.anon_inode_class` (eventfd/eventpoll/signalfd/timerfd/inotify).
- **Pre-tag sanity at this HEAD:** `CURRENT_PROJECT_VERSION` bumped to 534 consistently across the four app configs (the secondary target stays frozen at 529); the emulator core compiles (`ninja -C build ish`) and smoke-runs (pipes/fds via the new adhoc path verified). Recommended at tag time: run the on-device `/AOK/tests` guest regression suite on **both** roots (i386 + amd64), as in 533.

## Commit Range

```
c8d46dab docs: correct 533 notes to describe the vendored static liblzma
6719251e tests: expose mount_flags.c and clone_error_cleanup.c at /AOK/tests
950fa01d build: bump to 534 and raise iOS deployment target to 15
a6ac3631 app: fix About-screen crash on iOS 27 (static-table LLM section bounds)
d85a615b fs: show real fd types in /proc/<pid>/fd instead of anon_inode unknown
83888d4e app: fix "Invalid UIScreen coordinate space conversion" log spam
dc1bf508 workspace: add Music player applet (themed MP3-player UI)
a0902451 workspace: fix terminal window × close and shell-exit respawn
8e1b83e3 deps: vendor static liblzma (xz 5.8.3, decoders+encoders); drop system liblzma.tbd
5c82b953 exec: don't map a writable PT_LOAD's EOF-straddling last host page file-backed
9432757a fs/lock: guard file_lock_from_flock against NULL fd/fs op pointers
898daf59 platform/darwin: don't abort the app when host_statistics64 fails (/proc/meminfo crash)
23312f2c Minor fix
```
