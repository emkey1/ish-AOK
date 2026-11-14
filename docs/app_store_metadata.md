# App Store metadata

Draft copy for the App Store Connect fields, with Apple's limits noted. Written
2026-11-14. Everything here is a starting point to edit, not something to paste
unread -- marketing copy is the author's voice, not a generated one.

**One dependency runs through all of it:** the store name. See the assessment's
"Name and branding" -- if the fork cannot be listed as "iSH-AOK", the name,
subtitle and keywords below all change, and so does the first line of the
description.

---

## Name (30 characters)

```
iSH-AOK
```

Seven characters, so there is room if a longer form is ever wanted. Apple
matches on the name heavily, so "iSH-AOK: Linux Shell" (20) is the obvious
alternative if discoverability matters more than restraint.

## Subtitle (30 characters)

```
A Linux shell on your device
```
28 characters. Says what it is in words someone searching would use.

## Promotional text (170 characters, changeable without a review)

```
Five guest architectures, a Wayland desktop on the GPU, native ssh and git,
and an on-device AI chat that can read and run things in your filesystem.
```
148 characters. This field can be changed between releases without review, so
it is the right place for whatever the current release added.

## Keywords (100 characters, comma-separated, no spaces after commas)

```
linux,shell,terminal,ssh,unix,bash,developer,command,emulator,alpine,debian,console,sftp,git
```
93 characters. Deliberately excludes "iSH" (the name field already carries it)
and any trademark that is not ours.

## Description (4000 characters)

```
iSH-AOK runs a real Linux system on your iPhone or iPad.

It is not a remote terminal and not a sandbox around a few commands. A
processor is emulated in software, a Linux userland runs on top of it, and you
get a shell with a package manager behind it. Install what you like with apk or
apt. Nothing is jailbroken and nothing leaves your device unless you send it.

FIVE GUEST ARCHITECTURES

i386, x86_64, arm64, riscv64 and 64-bit little-endian PowerPC, all emulated.
Run binaries built for hardware you do not own.

A DESKTOP, IF YOU WANT ONE

A Wayland desktop -- labwc, foot, waybar -- composited on the device's own GPU
through a virtio-gpu render node, so Mesa draws on Metal. Or stay in the
terminal; most people do.

FAST WHERE IT COUNTS

ssh, scp, sftp, git, curl, an editor and most of coreutils are compiled into
the app as native code rather than emulated, so they run at full speed. Video
conversion uses the hardware encoder the Camera app uses.

BUILT FOR REAL USE

- Files app integration, so the guest filesystem is a location in Files
- Apple Shortcuts actions, including a headless Run Command
- Images drawn in the terminal with kitty's graphics protocol
- Suspend to disk: processes, open files and terminals survive the app being
  killed
- FUSE, namespaces, binfmt_misc, and a GPU render node
- An external display shows the terminal full size while the phone keeps the
  keyboard

AN AI CHAT THAT CAN ACTUALLY DO THINGS

An optional chat panel that can read files, edit them and run commands in your
Linux filesystem, under permissions you set per category. It works entirely
on-device with Apple's Foundation Models -- no account, no key, nothing sent
anywhere -- or with a provider you configure yourself. It is switched off until
you turn it on, and it asks before the first message of a chat leaves the
device.

PRIVACY

iSH-AOK collects nothing. There is no account, no analytics, no advertising and
no tracking of any kind. What your programs send is your business, and we never
see it.

iSH-AOK is open source, a fork of the iSH project, and is developed in the open
at github.com/emkey1/ish-AOK.
```

About 2,050 characters, well inside the limit, with room to add.

Three things it deliberately does not do: promise performance it cannot keep
(emulation is slow and the copy does not pretend otherwise), name a
distribution as supported beyond the two we ship, or carry a TestFlight link.

## Support URL

```
https://github.com/emkey1/ish-AOK/issues
```
The assessment already settled that this will do.

## Marketing URL (optional)

```
https://github.com/emkey1/ish-AOK
```

## Privacy policy URL

```
https://github.com/emkey1/ish-AOK/blob/working/docs/privacy-policy.md
```
See must-fix 3 for the hosting options if a cleaner URL is wanted later.

## Copyright

```
2026 Mike Miller
```

---

## Screenshots

Required: **iPhone 6.9"** and **iPad 13"**. Apple accepts one set per device
family and scales down, so those two sizes cover the listing.

Suggested set, in order, because the first two are what most people see:

1. The terminal, with something recognisable on screen (`uname -a`, a build
   running, `htop`).
2. The Workspace with several windows, which is the thing screenshots of other
   terminal apps cannot show.
3. The Wayland desktop.
4. LLM Chat mid-task, with a permission prompt visible -- it shows both the
   feature and that it asks.
5. Files app, with iSH-AOK as a location.

`fastlane/Snapfile` already exists for automating these, though it drives the
simulator and the desktop and GPU shots may be easier taken on a device.

**Not done.** These have to be taken and chosen by hand; a generated set would
be worse than none.
