# Release Notes Since `builds/iSH-AOK_565`

134 commits. **The app is now called AOK-iSH.** Native mode grew the most: it
has `apt` and a Wayland desktop through a companion Devuan filesystem, its own sshd,
more accounts, and more commands. Most of the rest is preparation for the App Store:
privacy manifests, a privacy policy, a consent screen before an AI chat sends
anything, and no private APIs. Among the fixes, a long paste arrives whole, the
Wayland desktop takes any keyboard layout, music plays again, and DNS works on an
IPv6-only network.

## Before you update

**Saved sessions from 565 will not restore.** An image is tied to the build that
saved it. The guest boots fresh, and the resume picker marks such an image "can't be
restored".

**The name changed, your data did not.** The home screen, Files, Settings, Shortcuts
and the documentation say AOK-iSH. The bundle and App Group identifiers are the same,
so this installs over iSH-AOK and keeps every filesystem. The location in Files is
renamed with it.

## Native mode

### apt, through a companion filesystem

A native root has no distribution and so no package manager. It can now keep a second
filesystem that has one: its **companion**, Devuan 6 arm64, installed beside it and
named `Devuan`.

```sh
sudo apt install htop      # the first time, offers to install the companion
htop                       # in a new login
```

- `apt`, `apt-get`, `apt-cache`, `apt-mark`, `dpkg` and `dpkg-query` work from the
  native root, and what apt installs runs by name. Native programs win where both
  roots have a command.
- A program from the companion runs inside it, emulated, and shares the native root's
  `/home` and `/tmp`, its `/etc/hosts`, `/etc/hostname` and `/etc/resolv.conf`, and its
  time zone. Your account is the same on both sides.
- **Remove a companion with `sudo sh /AOK/tools/manage-roots.sh remove Devuan --yes`**
  or from Filesystems in the app.

See `/AOK/docs/native-companion.md`. **Tested in the Mac command line, and the first
install on an M4 iPad Pro; not yet through a whole session on a device.**

### A Wayland desktop

The desktop for a native root is installed into the same companion and runs from
there, while the booted root stays native: **Settings → Wayland Desktop**, or
`sudo sh /AOK/tools/setup-native-wayland.sh`. Its terminal runs your account's own
shell, from your home. `setup-wayfire.sh`, `setup-xfce.sh`, `setup-games.sh` and
`select-desktop.sh` all work from the native root and act on the companion.

### sshd

`ssh` out of a native root was always there. **Settings → sshd** now runs a server
too: a switch, a port (1022 by default) and **Password Login** (off by default). The
switch takes effect at once and at every boot after. To add your first key, fetch it
with the client:

```sh
mkdir -p ~/.ssh
ssh you@your-machine 'cat ~/.ssh/id_ed25519.pub' >> ~/.ssh/authorized_keys
```

With **Settings → Crypto Accel** on, its default cipher runs on the crypto
accelerator. See `/AOK/docs/native-mode.md`.

### Accounts and commands

- **Settings → Default User → Add User…** adds an account to a native root after the
  first start, or a second one.
- **`sudo -S` and `sudo -p`** work, so does a piped `passwd`.
- **`ip`, `ifconfig`, `w`, `last` and `finger`** are native commands now. `login` gives
  the terminal to the user, so `last` shows a live session as "still logged in".
- `tput` works without a terminfo database, `git add` takes directories and globs, and
  an `awk` array read as a scalar is an error as in every other awk.
- `/etc/os-release` names the SmallCLUE build the root's commands come from.
- A plain (non-Workspace) terminal on a native root shows a shell. It used to show an
  empty console with no prompt.

## App Store preparation

None of this changes how the app works for a tester, except where it says so.

- **The AI chat asks before the first message leaves the device**, once per provider,
  naming the host and what is sent. On-device models are not asked about.
- **Privacy Policy** and **Legal** rows in Settings. Legal carries the written offer
  for the source of the GPL software in the bundled filesystems.
- **Privacy manifests** for the app and the Files extension.
- **The app no longer closes itself.** Suspend and Exit, and Quit Now when changing
  filesystem, stop the machine and end on a screen that says the app can be closed.
- **No private APIs.** The device's DNS servers are read through public calls only.
  One consequence: changing the DNS servers of a Wi-Fi network you are already on
  reaches the guest at the next network change, not at once.
- The bundle no longer carries upstream iSH's pinned Alpine 3.14 repository list, which
  was written into imported upstream roots at every boot.
- **Shortcuts and Siri are translated** into the nine other languages, as are the
  strings added since 564. A new build check keeps the translations in step.

## Fixes

- **A long paste arrives whole.** Anything over 4096 bytes was cut off: a 23 KB script
  arrived as about 4 KB.
- **The Wayland desktop takes any keyboard layout**, not only US: æ, ø, å, accented
  letters and other scripts, from a hardware keyboard or the soft keyboard's accent
  menu. Symbols a layout has on Option or on a different Shift level arrive as typed.
- **Music plays again.** On 565 a track at 44.1 kHz stopped the moment it started, and
  playing a folder could crash the app (M4 iPad Pro).
- **DNS on an IPv6-only network.** An IPv6 nameserver was dropped, so a network with
  only IPv6 resolvers left the guest with no names at all. A NAT64 network has not been
  tried.
- **`ping google.com` on a network with no IPv6 route** (glibc roots). Name lookups
  ranked IPv6 addresses first, and every packet was lost.
- **A tap brings the keyboard back** (TestFlight feedback on 564) when the terminal's
  page has been reclaimed by iOS or has not loaded yet. It used to take restarting the
  app.
- **A full-screen program no longer sits a row low** after the terminal grows, as when
  hiding the keyboard under `btop` in landscape.
- **No stray `^[[?1;2c#` or `#` above the first prompt** of a native zsh, and no
  `+q524742` at an ssh login.
- **The terminal waits for unlock to rebuild a page iOS reclaimed.** Rebuilding under
  the lock screen is the likely cause of the app being ended on one locked iPad
  (iPadOS 26.6, on 563 and 564). Only that iPad can confirm it.
- **A symlinked directory opens in Files**, so `bin`, `sbin` and `lib` on a merged-`/usr`
  root are folders, not unreadable files.
- **`suspend.sh` run by a native shell** no longer suspends again on resume.
- **A filesystem installed with `manage-roots.sh`** appears under `/AOK/roots` at once,
  not at the next launch.
- **`ktop` shows %CPU on a narrow terminal**, where the column was off the edge.
- **Typing a theme's name** no longer closes the keyboard on every character.
- **The Video Converter offers a Files result under the original's name.**
- **PSCAL roots have the native programs on `PATH` at an ssh login.**
- `strace` shows the accelerator system calls correctly.
- `tar t` and `ls -b` escape unprintable names as GNU does, in the app as in the
  command line.
- The Diagnostics report says where storage lives and whether Files accepted AOK-iSH
  ([#625](https://github.com/emkey1/ish-AOK/issues/625),
  [#676](https://github.com/emkey1/ish-AOK/issues/676)).
- The iPhone Duo and the A20 Pro iPad mini are named in the device tables.

## New tools

- **`/AOK/tools/ssh-stash.sh`** carries a login's `~/.ssh` between filesystems by name:
  `save`, `restore`, `list`.

## Testing

The full regression suite on the five release roots — Alpine 3.24 i386, amd64, arm64
and riscv64, and Devuan 6 arm64 with glibc — on this build:

| root | M5 Mac (command line) | A10X iPad (the app) |
|---|---|---|
| i386 | 459 passed | 462 passed |
| amd64 | 479 passed | 484 passed |
| arm64 (musl) | 396 passed | 401 passed |
| riscv64 | 388 passed | 393 passed |
| arm64 (Devuan, glibc) | 396 passed | 401 passed |

No failures on either. Every count is one more than 565's, except the iPad's Devuan
leg, which ran as root this time and so also ran `mount_cross_dev`. The iPad's extra
passes are tests the Mac skips.

The iPad is a 2017 12.9" iPad Pro on iPadOS 17.7, and each root was booted as the
running root, not reached through a chroot. Its i386 and amd64 roots are Alpine 3.23.3,
and its riscv64 root is Devuan 6 with glibc. The Mac ran the commit that routed
`if_nametoindex` for native programs; the iPad ran the build from just before the
version number changed to 566. Between the two, the changes were the local network
permission text, the VoiceOver workaround being limited to iOS before 17, and
documentation.

**No device on iOS 26 or 27 ran the suite for this build.** The ppc64le root was not
run. The e2e suite passed 7 of 7 from a freshly built test filesystem.

## Known issues

- **The companion's apt and desktop are new.** Programs outside `/home` and `/tmp` see
  the companion's files, not the native root's.
- **Removing a filesystem that `mount-root.sh` has mounted**: run
  `mount-root.sh --unmount NAME` first. Its shared directories are visible from
  outside until then.
- **The external display is untested on hardware.**
- **iOS can end the app seconds into a launch** (0xdead10cc), when it suspends the app
  while a guest's file change is being committed to the root's database.
- **Sandboxes cannot reach the network** through a veth: there is no forwarding or
  bridge yet.
- **ssh sessions still drop when the device locks**
  ([#631](https://github.com/emkey1/ish-AOK/issues/631)): iOS takes a suspended app's
  connections.
- **A native program's own threads share one task** and are not stopped when the
  program ends.
- **Native ssh prints "Unknown error: 111"** for a refused connection.
- **Xfce draws no desktop backdrop** on some setups: icons on black.
- **On distribution roots, `who` misses the app terminal's own login.**
- **ppc64le is experimental**: no speed work yet, and no POWER9 instructions.
