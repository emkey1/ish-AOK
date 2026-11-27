# App Store metadata

Draft copy for the App Store Connect fields, with Apple's limits noted. Written
2026-11-14.

**Live on App Store Connect since 2026-11-24** (sent through the API from this
file, en-US, version 1.3; build 567 attached since 2026-11-27, in place of 566): the name, subtitle,
promotional text, keywords, description, privacy policy URL and both screenshot
sets -- four each (terminal, Workspace, Files, LLM Chat). The Wayland desktop
shot was taken out of both: the desktop runs on a companion Devuan filesystem,
and the listing does not point at other distributions. The files stay in
`build/app-store-screenshots/`. The support URL there is still the repository's home page, not `/issues`.
Edit here first, then send, so the two stay the same. The revised subtitle, promotional
text, keywords and description were sent the same day, after the user read them.

**Also set 2026-11-24, at the user's word:** categories Developer Tools
(primary) and Utilities; the age-rating questionnaire, every content question
"none" and unrestricted web access "yes" -- 16+ on Apple's current tiers (the
API's old-scale field reports it as SEVENTEEN_PLUS); price Free, base territory
USA; available in 174 of 175 territories and new ones automatically, **France
left out** -- by choice since 2026-11-26: no ANSSI declaration will be filed
(assessment item 7); copyright
`2026 Mike Miller`. Content rights set 2026-11-25, at the user's word: "Yes,
it contains, shows or accesses third-party content" (`USES_THIRD_PARTY_CONTENT`)
-- the browser, curl and the AI chat reach sites the user picks, the catalogue
offers others' distributions, and the binary carries open-source code, whose
notices are on the app's Licences screen. **Still for the user, on the
website:** the App Privacy label ("Data Not Collected") and EU trader status. The App
Review notes (docs/app_store_review_notes.md, 3,174 characters) were sent too.
**Before submitting:** attach a build whose encryption answer is "yes" (567);
566 was answered "no", which the notes and the assessment contradict. Everything here is a starting point to edit, not something to paste
unread -- marketing copy is the author's voice, not a generated one.

**Renamed 2026-11-26: the app is AOK-Workstation.** The user moved off
"AOK-iSH" so that the store name no longer carries "iSH" (see the assessment's
"Name and branding"). The name, the description and the review notes were sent
to App Store Connect again the same day, at the user's word; the live text was
checked first and differed from this file only in the name. The privacy and
accessibility pages were republished too. So were the five screenshots that showed the
old name (the iPad `uname -a` line, both Workspace shots, both Files shots),
retaken the same way and put in the same places; the old files are in
`build/app-store-screenshots/old-aok-ish/`. TestFlight's beta app description
and beta review notes, which still said "iSH-Shell" and named distributions,
were rewritten and sent the same day, after the user read them. The description keeps "a fork of
the iSH project": stating where the app came from is how people who search for
iSH find it, and it puts no one else's name in the name or the keywords.

---

## Name (30 characters)

```
AOK-Workstation
```

15 characters. On the Home Screen the app is "Workstation"
(`CFBundleDisplayName`), because about 12 characters fit under an icon. Store
names must be unique and Home Screen names need not be. Searched on 2026-11-26:
no app in the US store (iPhone, iPad or Mac) is called "AOK-Workstation". An
iPhone app by Locstatt is called "Workstation", which does not stop us using
that as the Home Screen name.

## Subtitle (30 characters)

```
A Unix-style shell on device
```
28 characters. Revised 2026-11-24: not "Linux", which is the kernel, and AOK-Workstation
runs no Linux kernel -- it implements a useful subset of its interface.

## Promotional text (170 characters, changeable without a review)

```
A shell and about 140 commands built into the app, ssh and git among them, a
windowed Workspace, and an AI chat that can read and run things for you.
```
About 150 characters. This field can be changed between releases without review, so
it is the right place for whatever the current release added.

## Keywords (100 characters, comma-separated, no spaces after commas)

```
shell,terminal,ssh,sftp,git,zsh,command,cli,unix,linux,developer,console,coreutils,posix
```

Revised 2026-11-24: no distribution names (`alpine`, `devuan` are gone) and no
`emulator` -- the listing leads with the native root and does not point at
other distributions. `linux` stays as a search term only: keywords are not
shown, and the description says plainly that no Linux kernel runs. `unix`,
`linux` and `git` are other people's names; 2.3.7 is where an objection would
come from, and they are the ones to drop if Apple raises it.

## Description (4000 characters)

```
AOK-Workstation puts a Unix-style command line on your iPhone or iPad.

Open it and you have a shell and about 140 everyday commands -- ssh, scp, sftp,
git, curl, editors and most of coreutils -- compiled into the app itself, so
they run at full speed and there is nothing to download. Nothing leaves your
device unless you send it.

HOW IT WORKS

AOK-Workstation is not a remote terminal and not a virtual machine. It implements a
useful subset of the Linux system interface -- processes, files, pipes,
signals, terminals and networking -- inside the app, so familiar command-line
tools behave the way you expect.

A WORKSPACE, NOT JUST A TERMINAL

Arrange windows on desktops of their own: terminals with tabs, a file manager,
the MotePad editor, a web browser, music and video players, an image viewer, a
Markdown reader and a system monitor, all working on the same files.

CONVERT VIDEO AND AUDIO

The Media Converter re-encodes video on the hardware encoder the Camera app
uses, and converts audio to AAC, ALAC or WAV with the system's own codecs, from
Photos, the Files app or the app's own folders.

BUILT FOR REAL USE

- Files app integration, so your files are a location in Files
- A built-in ssh server, so you can work from your computer
- Apple Shortcuts actions, including a headless Run Command
- Images drawn in the terminal with kitty's graphics protocol
- Suspend to disk: processes, open files and terminals survive the app being
  killed
- On iPhone, an external display shows the terminal full size while the phone
  keeps the keyboard

IN YOUR LANGUAGE

The app speaks English, Simplified and Traditional Chinese, Japanese, Korean,
Spanish, French, German, Brazilian Portuguese and Russian, and the terminal
takes Chinese, Japanese and Korean input.

AN AI CHAT THAT CAN ACTUALLY DO THINGS

An optional chat panel that can read your files, edit them and run commands
for you, under permissions you set per category. On devices that support it,
it works entirely on-device with Apple's Foundation Models -- no account, no
key, nothing sent anywhere -- or with a provider you configure yourself. It is
switched off until you turn it on, and it asks before the first message of a
chat leaves the device.

PRIVACY

AOK-Workstation collects nothing. There is no account, no analytics, no advertising and
no tracking of any kind. What your programs send is your business, and we never
see it.

AOK-Workstation is open source, a fork of the iSH project, and is developed in the open
at github.com/emkey1/ish-AOK.
```

About 2,500 characters, inside the 4,000 limit. The lines are wrapped here for
reading only: the App Store keeps line breaks, so join each paragraph (and each
list item) into one line before it goes into App Store Connect.

Revised 2026-11-24 at the user's word, after a first version had been sent:

- It leads with what ships -- the native root and the commands built into the
  app -- and does not spotlight other Linux distributions: no apk or apt, no
  Alpine or Devuan, no guest architectures, and no desktop or FUSE, which need
  a distribution.
- It does not claim to be Linux, which is the kernel. AOK-Workstation runs no Linux
  kernel; it implements a useful subset of its interface, and says so.
- It does not mention jailbreaking, even to deny it: App Review has rejected
  metadata for the word alone, and "built into the app" already says it.
- It promises no performance beyond the native commands', and carries no
  TestFlight link.

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
https://emkey1.github.io/ish-AOK/privacy/
```
GitHub Pages, from the `gh-pages` branch; `tools/publish-pages.sh` (once
`tools/publish-privacy-policy.sh`, which still runs it) republishes it after
any change to `docs/privacy-policy.md`.

## Accessibility URL (optional)

```
https://emkey1.github.io/ish-AOK/accessibility/
```
`docs/accessibility.md`, published by the same `tools/publish-pages.sh`. Shown
on the product page beside the Accessibility Nutrition Label.

## Accessibility Nutrition Label (2026-11-25, for 567)

The same on iPhone and iPad: **yes** to Dark Interface, Larger Text,
Differentiate Without Color Alone and Reduced Motion; **no** to VoiceOver,
Voice Control, Sufficient Contrast, Captions and Audio Descriptions. The
reasons, and what would change each "no", are in `docs/accessibility.md` and
the TODO entry "Accessibility Nutrition Label". Larger Text and Differentiate
Without Color rest on 567 (fd918053b, 94e1e305b), not on 566.

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
5. Files app, with Workstation as a location (Files uses the Home Screen name).

`fastlane/Snapfile` already exists for automating these, though it drives the
simulator and the desktop and GPU shots may be easier taken on a device.

**Not done.** These have to be taken and chosen by hand; a generated set would
be worse than none.
