# Shortcuts: drive iSH-AOK from Apple's Shortcuts app

On iOS/iPadOS 16 or later, iSH-AOK shows up in Apple's Shortcuts app with
two kinds of actions: opening the app to a specific surface, and — the
useful one — running a shell command in your guest system and handing its
output to the rest of the shortcut, without the app ever coming to the
foreground.

## Run Command

The **Run Command** action executes one command line in the booted guest
and returns its merged stdout+stderr as text, ready to feed into Show
Result, share sheets, files, or any other Shortcuts action. It works from
automations too (time of day, arriving somewhere, NFC tags, Back Tap...),
and Shortcuts can run it while iSH-AOK is closed — iOS launches the app in
the background for the duration of the command.

Details worth knowing:

- The command runs as `-c` of the **native zsh** (`/AOK/native/zsh`), as
  root, as a fresh child of init — not inside any terminal session you may
  have open. If the native zsh cannot start, the action falls back to
  `/bin/sh`.
- stdin is `/dev/null`, `TERM=dumb`, and `PATH` is the standard system
  path plus `/AOK/persist/bin`.
- **Timeout** (default 20 s, max 120 s): when it expires the command is
  killed and the action fails, reporting any partial output. iOS gives a
  background launch limited runtime, so keep unattended commands well
  under a minute; for long jobs, open the app first.
- Output is capped at 256 KB.
- **Fail on Non-Zero Exit** (off by default): when on, a non-zero exit
  status fails the action (useful in automations that should stop on
  error); when off, you get the output either way.

The action is gated by **Settings → Shortcuts → Allow Shortcuts to Run
Commands** (on by default; also scriptable as
`/proc/ish/defaults/shortcuts_run_commands`). With the gate off, the
action fails with an error saying so, and runs nothing.

## Open actions

"Open iSH-AOK" takes a destination and brings the app up there directly:
the Workspace dashboard, the Session Shell, the system console, the
browser, themes, or boot images. Each destination is also available as an
App Shortcut with a Siri phrase, e.g. "Open session shell in iSH-AOK".

## The other direction: /dev/url

Everything above is Shortcuts calling *into* iSH-AOK. `/dev/url` is the
way back out: write a URL to it and iOS opens it, exactly as if you had
tapped a link.

    echo https://example.com > /dev/url      # opens Safari
    echo youtube:// > /dev/url               # opens the YouTube app
    echo 'shortcuts://run-shortcut?name=Goodnight' > /dev/url

That last form is the interesting one, because it closes the loop: a
shortcut can run a guest command, and a guest command can run a shortcut
— so anything you have automated on the phone is reachable from a shell
script. Any scheme an installed app claims works; `tel:`, `sms:`, `maps:`
and `mailto:` are all just URLs.

It is a device rather than a command so it composes the way a shell
expects — redirect into it, pipe into it, use it from a script with no
extra binary to install. Like `/dev/clipboard` and `/dev/location`, it
exists only in the app; the command-line build has no iOS to ask.

What it will tell you:

- The write returns only once iOS has answered, so a **successful write
  means the URL really was opened**. It is not a fire-and-forget that
  always reports success.
- A URL with no scheme is `EINVAL` — `echo youtube.com > /dev/url` is an
  error, `echo https://youtube.com > /dev/url` is not.
- `EPERM` means iOS refused: no installed app claims the scheme, or
  iSH-AOK was not in the foreground. iOS does not allow a background app
  to open URLs, so this will not work from a Shortcuts automation that
  runs while the app is closed.
- A trailing newline is trimmed for you, so plain `echo` is fine.
- Reading it gives nothing; there is no state to read back.

## A note on trust

A shortcut runs whatever command it was built with, as root. That is the
point — but treat shortcuts you import from other people like scripts you
found on the internet, and read what they run before running them. If you
never use this, turn the gate off in Settings.

`/dev/url` deserves the same care from the other side: anything running
in the guest can open a URL, which means it can launch apps and trigger
your shortcuts. It cannot do so silently — iOS switches away from
iSH-AOK to whatever it opened, so you always see it happen.
