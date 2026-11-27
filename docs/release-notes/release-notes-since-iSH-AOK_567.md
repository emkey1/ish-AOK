# Release Notes Since `builds/iSH-AOK_566`

62 commits. **The app is now called AOK-Workstation**, and "Workstation" on
the Home Screen. A fresh install now starts straight into the native filesystem,
with nothing to choose or download. Native mode can install PSCAL with `apt`, from
AOK-Workstation's own package repository. The LLM chat reads Agent Skills, the
Media Converter does audio, root archives open from the Files app, and the Wayland
window can start a desktop of your choosing. Settings follows the system's text
size, and a Licences page lists every licence built into the app. Among the fixes:
the pointer moves again on a software-rendered desktop, the iPad keyboard should no
longer crash the app as it moves, and Export container no longer freezes it.

## Before you update

**Saved sessions from 566 will not restore.** An image is tied to the build that
saved it. The guest boots fresh, and the resume picker marks such an image "can't be
restored".

**The name changed again; your data did not.** Menus, Settings, the in-app docs and
the book say AOK-Workstation. Places that list the app by its Home Screen name say
Workstation: the Files app's location, the Settings app (Settings → Workstation),
Shortcuts, Spotlight and Siri. The bundle and App Group identifiers are unchanged, so
this installs over 566 and keeps every filesystem.

## First launch

**A fresh install boots AOK-Workstation Native at once.** It used to open on the
Filesystems screen and wait for a choice. Now, with no filesystem at all, the app
makes the native filesystem (nothing is downloaded), makes it the default, and boots
it. Add a distribution later from **Boot Images**. An existing install is unchanged.

## Native mode

### PSCAL from AOK-Workstation's own apt repository

A native root's companion (the Devuan filesystem `apt` works in) now has a second
package source beside Devuan's: AOK-Workstation's own repository,
[emkey1/aok-apt](https://github.com/emkey1/aok-apt). Its first package is PSCAL:

```sh
sudo apt update
sudo apt install pscal     # pascal, clike, rea, aether, exsh, pscalvm
pascal hello.pas           # in a new login, like anything apt installs
```

- Nothing is installed unless you ask. The source is added when the companion is set
  up, or at the first `sudo apt` for a companion set up before 567.
- It is signed with the repository's own key, and trusted for that key only:
  ed25519 `D127 47AE 9E6F 55A7 C139  BD6F E16C 5C31 AF98 5A69`.
- `sudo sh /AOK/tools/aok-apt.sh --remove` takes the source away, and it stays away.
  The same script adds it to any other Devuan 6 filesystem.
- Like everything apt installs, PSCAL runs emulated in the companion. On an M4 iPad
  Pro a small program in each language ran in 44-110 ms. SDL graphics are not in this
  build.

See `/AOK/docs/native-companion.md`.

### The desktop

- **A companion now sees `/AOK/bundled`**, so native mode's desktop uses the bundled
  wf-panel, wayvnc and pixman shim, as a Devuan root does. An idle Wayfire desktop on
  the A10X now spends a fraction of the CPU it did (below).
- `ssh` and other native programs report a refused, timed-out or unreachable
  connection in words. A refused connection used to read "Unknown error: 111".

## Wayland desktops

- **A desktop command of your own (#674).** `sudo sh /AOK/tools/select-desktop.sh
  --command 'phoc -E phosh'` makes the Wayland window start any compositor or session
  you have installed, in place of labwc, Wayfire, Xfce or the native desktop. The
  command runs through `/bin/sh`, so it can quote arguments and set variables. When
  the window closes, everything the command started ends with it. Choosing a named
  desktop again clears it.
- **The pointer moves again on a software-rendered desktop.** Since 566, a tap, a drag
  and Touchpad Mode did nothing there, while keys typed. A desktop on the GPU was not
  affected.
- **Appearance and Resolution change the running desktop** instead of waiting for a
  Reconnect, where the desktop allows it. On a 2x iPad, "2x" is no longer listed beside
  "Native (2x)", which was the same size.
- **The window's menu has a button you can see**, an ellipsis circle beside Paste,
  instead of a faint "⋯". Touchpad Mode, Resolution and the keyboard toggles are there.
- **A window that failed tries again** once a desktop is installed, and when it comes
  back to the front from the dock, the Launcher or an `open`.
- **An idle desktop stays idle.** The panel clock no longer redraws every second when
  it shows no seconds, and the session's watch loop sleeps 15 s at a time while still
  noticing at once when the desktop exits. On the A10X, 30 s of an idle native desktop
  cost 0.5-1.2 s of panel CPU before and under 0.1 s now.
- **Settings → Wayland Desktop → More Programs** says whether Games, Tools, X11
  Programs and GPU Drivers are installed, partly installed or not.
- `workspace.md` and `keyboard-shortcuts.md` say how to move a window by any part of
  it: Alt and a drag (the key row's alt stays held until you tap it again).

## Media Converter: audio (#678)

The Video Converter is now the **Media Converter**, and it converts sound with the
system's codecs:

- **In the Workspace**, a video can become HEVC, H.264, AAC, ALAC or WAV, and an
  audio file AAC, ALAC or WAV. Files offers audio, and the File Manager has
  **Convert Audio…**. A Photos video's sound goes to the export picker.
- **`transcode`** writes `.m4a` (AAC, or ALAC with `-c alac`), `.caf`, `.wav` and
  `.aif`, from audio files or from a video's sound. It keeps the sample rate, channels
  and bit depth, and copies title, artist and album into an `.m4a`. Formats the system
  cannot write (`.mp3`, `.flac`, `.ogg`, `.opus`) are refused with what to use instead.

Tested in the simulator; not on a device.

## LLM Chat: Agent Skills

With tools on, the chat reads [Agent Skills](https://agentskills.io): folders holding
a `SKILL.md`, the format Claude Code, Codex and other agents use. Every prompt lists
the skills found, and the model reads one in full when a task calls for it.

- It looks in `.agents/skills` and `.claude/skills` in the working directory and each
  folder above it, then in your home directory, then in `/AOK/persist/skills`, which
  every filesystem shares and the Files app shows as Workstation › Persist › skills.
- `/skills` lists what was found, what the model is given, and why anything was left
  out. **Settings → LLM Client → Skills** turns it off.
- A skill is instructions the model follows: read one from someone else before you
  put it where the chat looks.

Tested in the simulator with a small local model, and on an A10X iPad Pro with a
larger one. See `/AOK/docs/llm-chat.md`.

## Filesystems from other apps

**A root filesystem archive opens in AOK-Workstation** from the Files app, Safari's
downloads, AirDrop or any app's share sheet: `.tar`, `.tar.gz`, `.tar.xz` and
`.tar.bz2`. The app asks whether to add it as a new filesystem; the one you are using
is not changed. See `/AOK/docs/roots.md`.

## Settings and accessibility

- **Settings, Filesystems and Diagnostics follow the system's text size** on every
  iPhone and outside the Workspace on an iPad, up to the largest accessibility sizes.
  Their rows wrap and grow to fit.
- **A held key is outlined**, not only shaded: ctrl in the terminal's key row, ctrl,
  alt and ❖ in the Wayland window's.
- **Settings → Licences**: every licence built into the app, in full, with search.
  That is 419 entries: AOK-Workstation itself, 25 components and the Rust crates and
  standard library the app compiles. The Legal screen points to it.
- **Export container (in the advanced debugging options) no longer freezes the
  app.** It copied every filesystem on the
  main thread until iOS killed the app. It now runs in the background with progress
  and Cancel, clones files instead of copying them, and says where the copy went.

## Fixes

- **The iPad keyboard should no longer crash the app as it moves.** On iPads with a
  floating keyboard assistant, the terminal swapped its key row while the keyboard
  was moving, and UIKit raised as the move finished (six reports, 564 and 565). The
  row now changes only once the keyboard has settled. That placement exists only on
  Apple Intelligence iPads, so whether this is the whole fix will show on 567.
- **A native program that renames a file renames it in the guest.** Native Helix
  saved through `renameat`, which went to the host's paths (found by 566's release
  checks; the GitHub IPA of 566 had the fix, the TestFlight one did not). A save from
  native `hx` checked on an M4 iPad Pro: the file replaced, no temporary file left.
- **The Photos permission text names the Media Converter**, in all ten languages.

## Behind the scenes

- The App Store listing, review notes and TestFlight texts were rewritten for the name
  and for what the app is: a Unix-style environment on the device that implements a
  useful part of the Linux interface. An accessibility page is published for the
  listing.
- `ITSAppUsesNonExemptEncryption` is now NO, so uploads no longer ask the encryption
  questions. France stays out of the App Store, by choice.
- `tools/testflight-feedback.py` reads TestFlight feedback and the comments testers
  leave on crashes from App Store Connect, which the Organizer does not show.
- `tools/check-licences.py` keeps the Licences page in step with what is built.
- The Workspace test hooks (Debug builds only) can drive the LLM chat, so a device
  test needs no taps.
- `docs/pscal_native_plan.md` sizes running PSCAL natively. It is shelved.

## Tested

The guest regression suite on all five roots, on an M5 MacBook Pro and on an M4 iPad
Pro, booting each root on the iPad. Both machines ran the same tests, and nothing
failed on either:

| Root | Tests | Failures |
|---|---|---|
| i386 Alpine | 468 | 0 |
| amd64 Alpine | 488 | 0 |
| arm64 Alpine | 405 | 0 |
| riscv64 Alpine | 397 | 0 |
| arm64 Devuan (glibc) | 405 | 0 |

Nine to fourteen tests a root skip themselves, such as one architecture's tests on
another, or swap controls only the command-line build can drive. Also clean: the
native-mode leg (115 checks, including the new package-source ones), the end-to-end
suite, and the release checks (`tools/check-all.py`).

The native-mode leg caught one regression from the rename before the tag: a native
root stopped recording the links it makes, so links to programs that leave the build
were never removed. It is fixed in this build.
