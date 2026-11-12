# Release Notes Since `builds/iSH-AOK_564`

30 commits. The terminal draws pictures now: programs that use kitty's graphics
protocol, such as `kitten icat` and `chafa`, show real images. Videos convert on the
device's hardware encoder, from the terminal with `/AOK/native/transcode` or with
buttons in the Workspace's new Video Converter. iSH-AOK has its own set of app
icons. Among the fixes, the music player no longer crashes the app when a track
changes format, terminal text no longer vanishes after a tty switch, and Suspend to
Disk no longer loses the session while the Wayland desktop is running.

## Before you update

**Saved sessions from 564 will not restore.** An image is tied to the build that
saved it. The guest boots fresh, and the resume picker marks such an image "can't be
restored".

## Images in the terminal

The terminal draws images sent with **kitty's graphics protocol**, the one `kitten
icat`, `chafa`, `timg` and a number of file managers use. An image appears at the
cursor, scrolls with the text, and goes when its lines are cleared; `clear` and
`reset` take images off with the text, BusyBox's `clear` included.

```sh
sudo apk add kitty-kitten chafa    # Alpine; on Devuan: sudo apt install chafa
kitten icat picture.png
chafa -f kitty -s 40x12 photo.jpg
```

The terminal also reports its size in pixels now, through `TIOCGWINSZ` and `CSI 14 t`,
`16 t` and `18 t`; without that, `kitten icat` refused to start. Unicode placeholders,
animation and drawing under the text are not supported. See
`/AOK/docs/terminal-images.md`.

## Converting videos on the hardware encoder

`ffmpeg` in the guest encodes in emulated software; a Discord report measured H.265 at
0.005x real time. **`/AOK/native/transcode`** hands the job to the encoder the Camera
app uses, and runs at full speed whatever the root's architecture:

```sh
/AOK/native/transcode IMG_1234.MOV small.mp4          # HEVC, same size
/AOK/native/transcode -s 1080 -c h264 in.mov out.mp4  # 1080p H.264
```

It keeps orientation, the creation date and location, and HDR when the output is HEVC
(H.264 is tone-mapped to SDR). In an `.mp4`, audio that is not AAC, such as a Live
Photo's or spatial audio, is re-encoded to stereo AAC so the file plays everywhere.
Ctrl-C cancels and removes the partial file. See `/AOK/docs/transcode.md`.

The Workspace's **Video Converter** applet does the same with buttons. It takes a video
from Photos (without asking for access to your library), from the Files app, or from
the guest (**Convert Video…** in the File Manager). A video from Photos goes back to
Photos with the original's date and place, and **Delete Original** then frees the space
(iOS asks first, and the original stays in Recently Deleted). See
`/AOK/docs/workspace.md`.

On the A10X iPad, a 10-second HDR clip from an iPhone 14 converted to HEVC in 2.2
seconds, and the guest and Photos sources both worked from start to finish. **The Files
source and its save picker have run only in the simulator.**

## New app icons

Twelve icons drawn for iSH-AOK replace the twenty alternates inherited from upstream
iSH: Amber Phosphor, Blueprint, Boot Log, A-OK, Desktop, Gadget Chain, Three Guests,
Mono, Prompt, Silicon, Sunset and Suspend. **If you chose one of the old icons, you
keep a working one:** the update moves your Home Screen to the closest new icon, with
no prompt, and Settings shows that icon as chosen. Checked in the simulator by
installing this build over 564 with an old icon chosen.

## Crash fixes

- **The music player could crash the app as a new track loaded** (an iPhone 12 Pro on
  iOS 27.2, five times in 35 minutes on 564). Changing to a track in another format
  reconnected the audio engine while it ran, and iOS raised an exception when it could
  not. It now reconnects only when the format changes, with the engine stopped, and a
  track it still cannot play is skipped. The iOS 27.2 crash was not reproduced; ten
  changes between three formats while playing ran cleanly on the A10X.

## Other fixes

- **Suspend to Disk no longer loses the session while the Wayland desktop runs.** In
  the background, iOS refuses GPU work, so the app holds it, and a desktop thread
  waiting on the GPU could not be stopped for the save: on the A10X, with the screen
  locked, the save gave up after 5 seconds, twice in a row. Such a process is now left
  out, as the desktop always is, and the rest is saved. On the A10X, locked with the
  desktop up, the save named the waiting thread and saved 40 processes in 5 seconds.
- **Terminal text no longer vanishes** (TestFlight feedback on 564, iOS 27.0.1). After a
  tty switch, or after iOS rebuilt a terminal's page, the text could sit below the
  screen with only the scroll bar showing, and the prompt could fall under the
  keyboard.
- **Settings has its Done button back** (TestFlight feedback on 564). On an iPhone in
  landscape, Settings fills the screen and cannot be swiped away, which left no way to
  close it.
- **The Wayland mirror on an external display keeps drawing**
  ([#540](https://github.com/emkey1/ish-AOK/issues/540)). On 564 it showed one frame and
  never another (iPhone 16 Plus, iOS 27.0.1). The fix has run only in the simulator;
  `/AOK/docs/external-display.md` now covers starting the desktop for the display and
  the text size there.
- **A terminal opened with a command keeps escape sequences whole.** Output that arrives
  before the page loads was kept as its last 16 KB, which could cut an image in half
  and print its data as text. It now keeps up to 1 MB and never splits an escape
  sequence.
- **VoiceOver hints on the Done, Cancel and Close buttons**
  ([#663](https://github.com/emkey1/ish-AOK/pull/663)), translated into the nine
  languages.
- Two small speed-ups in string handling
  ([#672](https://github.com/emkey1/ish-AOK/pull/672),
  [#675](https://github.com/emkey1/ish-AOK/pull/675)).

## Testing

The full regression suite on the five release roots — Alpine 3.24 i386, amd64, arm64
and riscv64, and Devuan 6 arm64 with glibc — on this build:

| root | M5 Mac (command line) | A10X iPad (the app) |
|---|---|---|
| i386 | 458 passed | 461 passed |
| amd64 | 478 passed | 483 passed |
| arm64 (musl) | 395 passed | 400 passed |
| riscv64 | 387 passed | 392 passed |
| arm64 (Devuan, glibc) | 395 passed | 399 passed |

No failures on either. Every count is the same as 564's, and the iPad's extra passes are
tests the Mac skips.

The iPad is a 2017 12.9" iPad Pro on iPadOS 17.7, and each root was booted as the
running root, not reached through a chroot. Its i386 and amd64 roots are Alpine 3.23.3,
and its riscv64 root is Devuan 6 with glibc. The Devuan arm64 leg ran as the default
user, and the one test that then skipped for want of root, `mount_cross_dev`, passed as
root; the other legs ran as root. The iPad ran the build from just before the
version number changed to 565; since then only documentation has changed.

**No device on iOS 26 or 27 ran the suite for this build.** On iOS 27 the new features
have run only in the simulator. The ppc64le root was not run. The e2e suite passed 7 of
7 from a freshly built test filesystem.

## Known issues

- **The external display is untested on hardware** (see above).
- **iOS has ended the app on one locked iPad** (iPadOS 26.6, on 563 and again on 564),
  for drawing while the screen was locked, as the automatic save ran. It did not happen
  on the A10X; the cause is not known yet.
- **iOS can end the app seconds into a launch** (0xdead10cc), when it suspends the app
  while a guest's file change is being committed to the root's database.
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
