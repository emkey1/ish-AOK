# Shortcuts: drive iSH-AOK from Apple's Shortcuts app

iSH-AOK adds two actions to Apple's Shortcuts app:

- **Run Command** runs a shell command in your guest system and hands its
  output to the rest of the shortcut. The app never has to come to the
  foreground, so it works from automations, Siri, the Home Screen, NFC tags
  and Back Tap.
- **Open iSH-AOK** brings the app up on a particular screen: the Workspace,
  the Session Shell, the system console, the browser, themes or boot images.

Going the other way, a guest program can open a URL, including a
`shortcuts://` link that runs one of your shortcuts. See
[/dev/url](#the-other-direction-devurl) below.

This page is about Apple's Shortcuts app. Three other things in iSH-AOK
have similar names:

- hardware-keyboard shortcuts are in
  [keyboard-shortcuts.md](keyboard-shortcuts.md);
- **Quick Actions** is a Workspace applet: a panel of buttons that open
  common tools (Layout Manager, Desktops, Session Shell, System Console,
  Sessions, Storage, Themes, Boot Images, Clock and, on iPad, New
  Workspace). `ws-quickactions` opens it. See [workspace.md](workspace.md);
- saved text you insert into a terminal is covered in
  [snippets.md](snippets.md).

## What you need

- **iOS or iPadOS 16 or later.** iSH-AOK itself runs on iOS 15, but the
  actions and Siri phrases only exist on 16 and later.
- **A root that has booted at least once.** Run Command boots the guest if
  it isn't running yet, but it cannot answer first-run setup for you.
- For Run Command only, **Allow Shortcuts to Run Commands** must be on. It
  is under **Settings → Shortcuts**, and it is **on by default**. The
  setting's footer says what it controls:

  > When enabled, the Shortcuts app's "Run Command" action can run shell
  > commands in the guest system without opening iSH-AOK.

  It is also `/proc/ish/defaults/shortcuts_run_commands`, which root in the
  guest can read and write (see [proc-ish.md](proc-ish.md)):

  ```sh
  cat /proc/ish/defaults/shortcuts_run_commands     # true or false
  echo false > /proc/ish/defaults/shortcuts_run_commands
  ```

  Open iSH-AOK does not depend on this setting.

To find the actions, open a shortcut in the Shortcuts app, tap **Add
Action**, and search for "iSH-AOK" or open **Apps → iSH-AOK**.

## The actions at a glance

| action | opens the app? | gated by the setting? | returns |
| --- | --- | --- | --- |
| Run Command | no; runs in the background | yes | the command's output, as text |
| Open iSH-AOK | yes | no | nothing |

## Run Command

The action reads **Run *Command* in iSH-AOK**. Timeout and Fail on Non-Zero
Exit are in its expanded options.

### Parameters

| parameter | type | default | allowed | what it does |
| --- | --- | --- | --- | --- |
| **Command** | text | none (required) | any text that is not blank | the command line to run. Leading and trailing spaces and newlines are trimmed. It can span several lines, and it can contain Shortcuts variables |
| **Timeout (seconds)** | number | 20 | 1 to 120 | how long the command may run before it is killed |
| **Fail on Non-Zero Exit** | on/off | off | | when on, a non-zero exit status makes the action fail instead of returning the output |

There is no input parameter. The command's standard input is `/dev/null`,
so you cannot pipe the shortcut's input into it. To get Shortcuts data into
a command, put it in the Command text; see
[Passing text into a command](#2-pass-text-from-a-shortcut-into-a-command)
for a safe way to do that.

### What it returns

When it succeeds, the action returns one piece of **text**: everything the
command wrote to standard output **and** standard error, merged in the order
it was written. If the command printed nothing, the text is empty.

The exit status is not returned separately. With Fail on Non-Zero Exit off
(the default) you get the output whatever the status was. To act on the
status, see [recipe 4](#4-branch-on-the-exit-status).

### How the command runs

By default the command runs **as root**:

| | |
| --- | --- |
| program | `/AOK/native/zsh -c '<your command>'`, the [native zsh](native-programs.md). If it cannot be started, the action uses `/bin/sh -c` instead |
| user | root (uid 0) |
| working directory | `/`, so `cd` first or use absolute paths |
| umask | `022` |
| standard input | `/dev/null`: anything that reads input gets end-of-file at once |
| standard output and error | both captured together; see [Output](#output) |
| environment | exactly `PATH=/AOK/persist/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`, `HOME=/root`, `TERM=dumb`, `LANG=C.UTF-8` and nothing else |
| shell type | neither a login shell nor an interactive one, so `~/.zprofile`, `~/.zshrc` and your aliases are not read |
| process | a new child of init, in its own session and process group. It is not part of any terminal you have open, though it runs in the same guest: it sees the same files and processes, and shows up in `ps` while it runs |

The native zsh runs on the host CPU rather than under emulation, so the
shell starts quickly; the programs your command runs are emulated as usual.

**With Open Everything as Default User on.** If **Settings → Default User →
Open Everything as Default User** is on, and the root has a UID 1000
account in `/etc/passwd`, the command runs as that account instead:

    /bin/su - <account> -c '<your command>'

That gives the command the account's uid, gid, groups and home directory,
and runs it in the account's own login shell, which sets `PATH`, `HOME` and
the rest the way a login does. There is no native-zsh shortcut and no
fallback to root on this path: if `/bin/su` cannot be started, the action
fails. If `su` starts but refuses the account, you get `su`'s own error
message as output and its exit status.

If the setting is on but the root has **no** UID 1000 account, the command
runs as root, as though the setting were off. The setting's footer in
Settings tells you which case you are in.

### Timeout

The timeout counts from the moment the command starts, not from when the
shortcut started, so booting the guest does not use it up. When it runs out,
iSH-AOK kills the command and everything in its process group with SIGKILL,
waits a fifth of a second for any last output, and the action fails with
"The command timed out after N seconds", followed by up to the first 1024
characters of what the command had printed.

iOS also limits how long an app launched in the background may keep
running, whatever the timeout says. Keep unattended commands short, and open
the app first for long jobs.

### Output

- Output is capped at **256 KB**. A command that reaches the cap is stopped
  there and killed, so the action usually fails with "The command was killed
  by signal 9". You do not get the first 256 KB. For big output, write it to
  a file and return a summary.
- The output must be valid **UTF-8**. If it is not (binary data, or text in
  another encoding), the action returns empty text.
- Error messages include at most the first 1024 characters of output,
  followed by "…".

The action waits until **every** copy of the command's output is closed, not
just until the shell exits. A background job that inherits the output keeps
the action waiting until the timeout:

```sh
sleep 60 &                              # the action waits, then times out
nohup sleep 60 >/dev/null 2>&1 &        # the action returns at once
```

A background job started like the second line keeps running after the
action returns, but only for as long as iOS lets the app run. When iOS
suspends the app, the job is suspended with it.

### Errors

The action fails, stopping the shortcut, with one of these messages:

| message | why |
| --- | --- |
| Running commands from Shortcuts is turned off. Enable "Allow Shortcuts to Run Commands" in iSH-AOK's settings. | the setting is off. Nothing ran |
| The command is empty. | Command was blank, or only spaces and newlines |
| The guest system could not boot (error N). Open iSH-AOK once to finish setup. | the guest could not be started; see [Troubleshooting](#troubleshooting) |
| Could not start the command (error N). Is the guest system booted? | neither `/AOK/native/zsh` nor `/bin/sh` could be started |
| Could not start the command as user "NAME" (error N). The "Open Everything as Default User" setting runs commands via /bin/su -- if su is missing or that account cannot log in, disable the setting or fix the account. | Open Everything as Default User is on and `/bin/su` could not be started |
| The command timed out after N seconds. Partial output: … | the timeout ran out |
| The command was killed by signal N. Partial output: … | the command was killed by a signal. Signal 9 usually means it hit the 256 KB output cap |
| The command exited with status N. Output: … | Fail on Non-Zero Exit is on and the status was not 0 |

The "Partial output" and "Output" parts are left out when there was no
output. The error numbers are negative errno values, as in Linux: -2 means
a file was not found, and -11 means try again (see
[Troubleshooting](#troubleshooting)).

### When the app is closed, in the background, or already open

- **Closed.** iOS launches iSH-AOK in the background to run the action.
  iSH-AOK boots your default root, the same one it would boot if you opened
  the app, and then runs the command. Expect that first run to take longer.
- **Saved sessions.** If you use [Suspend to Disk](suspend.md) and there is
  a saved session, a background boot **resumes the newest saved session
  without asking**, because no one is there to answer the question.
- **The resume question is on screen.** If the app is open and waiting for
  you to choose a saved session, nothing may boot until you answer. The
  action fails with "The guest system could not boot (error -11)". Answer
  the question and run the shortcut again.
- **Already running.** The command runs in the guest that is already up.
- **While it runs.** iSH-AOK asks iOS to keep it running until the command
  finishes. Once it has, and if the app is still in the background, iOS is
  free to suspend it again.

### One at a time

Run Command actions run **one at a time**, in the order they arrived. A
second shortcut that starts while one is running waits for the first to
finish. Its timeout starts only when its own command starts, but iOS's
background time limit does not wait with it.

## Open iSH-AOK

**Open iSH-AOK** brings the app to the foreground on the screen you choose.
It has one parameter, **Destination**, which defaults to **Workspace
Dashboard**:

| Destination | what you get |
| --- | --- |
| Workspace Dashboard | the window switches to the Workspace |
| Session Shell | the window switches to the terminal and shows the session's shell |
| System Console | the window switches to the terminal and shows the system console |
| Browser | the Workspace, with the Browser applet opened |
| Themes | the Workspace, with the Themes applet opened |
| Boot Images | the Workspace, with the Boot Images (filesystems) applet opened |

If iSH-AOK already has a window, that window changes to the destination, on
iPhone and iPad alike. On iPad the window used is the one in front, or if
none is in front, the first one. Only when no window is open does iSH-AOK
open a new one at the destination.

Switching to the Workspace or to the terminal keeps what was there:
a Workspace you already had comes back as you left it, and so does a
terminal session. The first switch to the terminal in a window that started
in the Workspace starts a new session.

The action does nothing while the app is in recovery mode, set to open
Diagnostics at launch, or waiting for you to pick a first root. Those
screens keep the window.

## Siri phrases and App Shortcuts

Each destination of Open iSH-AOK is also a ready-made App Shortcut. They
show up as tiles under iSH-AOK in the Shortcuts app and in Spotlight, with
no setup, and Siri understands these phrases:

| App Shortcut | phrases |
| --- | --- |
| Dashboard | "Open workspace in iSH-AOK", "Show dashboard in iSH-AOK" |
| Session Shell | "Open session shell in iSH-AOK", "Open shell in iSH-AOK" |
| System Console | "Open system console in iSH-AOK", "Open console in iSH-AOK" |
| Browser | "Open browser in iSH-AOK", "Show browser in iSH-AOK" |
| Themes | "Open themes in iSH-AOK", "Show themes in iSH-AOK" |
| Boot Images | "Open boot images in iSH-AOK", "Show boot images in iSH-AOK" |

Run Command has no built-in phrase, because it needs a command. Build a
shortcut around it and give it a name, and Siri will run it when you say
that name.

If the tiles or phrases are missing just after you install the app, open
iSH-AOK once. It registers them again every time it launches.

## Recipes

Each recipe is a shortcut you build in the Shortcuts app. The **bold**
names are Shortcuts actions.

### 1. Show the guest's uptime in a notification

1. **Run Command**: `uptime`
2. **Show Notification**, with the Run Command result as its body.

Any short report works the same way: `df -h /`, `free -m`,
`cat /proc/loadavg`.

### 2. Pass text from a shortcut into a command

The command has no standard input, so Shortcuts data has to go in the
command text. Pasting a variable straight into the command is fragile: a
quote, `$` or `;` in it becomes shell syntax. Encode it first:

1. **Ask for Input** (or whatever produces your text).
2. **Base64 Encode** the result.
3. **Run Command**, with the Base64 Encode result placed between the
   single quotes:

   ```sh
   printf %s 'BASE64-RESULT' | base64 -d | wc -w
   ```

Base64 output contains only letters, digits, `+`, `/` and `=` (and line
breaks if you ask for them), so nothing in it can break out of the single
quotes. Replace `wc -w` with whatever should read the text, such as
`>> /root/notes.txt` or `| tr a-z A-Z`.

### 3. A nightly backup that stops on error

1. In the Shortcuts app, **Automation → New Automation → Time of Day**, and
   set it to run without asking.
2. **Run Command**:

   ```sh
   /root/bin/backup.sh >/root/backup.log 2>&1; tail -n 5 /root/backup.log
   ```

   with **Fail on Non-Zero Exit** on and **Timeout** raised to suit, up to
   120.
3. **Show Notification** with the result.

`tail` exits 0, so this form always succeeds. To make the automation stop
when the backup fails, end with the backup's own status instead:

```sh
/root/bin/backup.sh >/root/backup.log 2>&1; s=$?; tail -n 5 /root/backup.log; exit $s
```

With Fail on Non-Zero Exit on, a failed backup stops the shortcut and the
error shows the last lines of the log.

Keep the job well inside the time iOS gives a background app. A backup that
needs minutes should be started from an open terminal, or started in the
background as described in [Output](#output).

### 4. Branch on the exit status

Leave Fail on Non-Zero Exit off and print the status as the last line:

```sh
pgrep -x sshd >/dev/null; echo $?
```

1. **Run Command** with that command.
2. **If** the result **is** `0` (sshd is running), do one thing;
   **Otherwise**, do another.

### 5. Open the Session Shell from an NFC tag, Back Tap or the Action button

- **NFC:** **Automation → New Automation → NFC**, scan the tag, then add
  **Open iSH-AOK** with Destination **Session Shell**.
- **Back Tap:** create a shortcut holding **Open iSH-AOK** (or use the
  ready-made **Session Shell** App Shortcut), then pick it in iOS Settings
  under Accessibility → Touch → Back Tap.
- **Action button**, on devices that have one: choose **Shortcut** in iOS
  Settings → Action Button and pick it.

### 6. Copy a command's output to the clipboard

1. **Run Command**: `df -h`
2. **Copy to Clipboard**.

**Save File**, **Quick Look** and **Share** take the result the same way.

## The other direction: /dev/url

Everything above is Shortcuts calling *into* iSH-AOK. `/dev/url` is the way
back out: write a URL to it and iOS opens it, exactly as if you had tapped a
link.

    echo https://example.com > /dev/url      # opens Safari
    echo youtube:// > /dev/url               # opens the YouTube app
    echo 'shortcuts://run-shortcut?name=Goodnight' > /dev/url

That last form is the interesting one, because it closes the loop: a
shortcut can run a guest command, and a guest command can run a shortcut, so
anything you have automated on the phone is reachable from a shell script.
Any scheme an installed app claims works; `tel:`, `sms:`, `maps:` and
`mailto:` are all just URLs.

It is a device rather than a command so it composes the way a shell expects:
redirect into it, pipe into it, use it from a script with no extra binary to
install. Like `/dev/clipboard` and `/dev/location`, it exists only in the
app; the command-line build has no iOS to ask.

What it will tell you:

- The write waits up to 5 seconds for iOS to answer, so a **successful write
  means the URL really was opened**. It is not a fire-and-forget that always
  reports success.
- A URL with no scheme is `EINVAL`: `echo youtube.com > /dev/url` is an
  error, `echo https://youtube.com > /dev/url` is not.
- A URL iOS cannot parse is also `EINVAL`. Spaces are the usual cause, so
  percent-encode them: `name=Good%20Night`, not `name=Good Night`.
- A URL longer than 8192 bytes is `EINVAL`.
- `EPERM` means iOS refused: no installed app claims the scheme, or iSH-AOK
  was not in the foreground. iOS does not allow a background app to open
  URLs, so `/dev/url` does not work from inside a Run Command action, nor
  from anything else that runs while the app is in the background.
- `ETIMEDOUT` means iOS did not answer within those 5 seconds. The write is
  bounded on purpose: a guest write must never become an unkillable wait on
  the UI thread.
- Trailing spaces and newlines are trimmed, so plain `echo` is fine. Writing
  only a newline does nothing and succeeds.
- Reading it gives end-of-file; there is no state to read back.

## Security

- **Who can run a command.** Any shortcut on the device that uses Run
  Command: ones you build, ones you import, automations (including ones set
  to run without asking), and Siri saying a shortcut's name. Nothing asks
  you first.
- **As whom.** Root in the guest, unless Open Everything as Default User is
  on **and** the root has a UID 1000 account. Guest root can read and change
  everything in the guest, including any host folders you have mounted into
  it.
- **The gate.** Allow Shortcuts to Run Commands is the off switch. It is on
  by default, so if you never use Run Command, turn it off. Guest root can
  also change it through `/proc/ish/defaults/shortcuts_run_commands`. That
  makes it a convenience for you, not a barrier against something that is
  already root in the guest.
- **Imported shortcuts.** A shortcut runs whatever command it was built
  with. Treat shortcuts from other people like scripts you found on the
  internet, and read the Command field before running one.
- **Shortcuts data in commands.** A variable inserted into the Command text
  becomes shell syntax. Encode untrusted text as in
  [recipe 2](#2-pass-text-from-a-shortcut-into-a-command).
- **Open iSH-AOK** only changes which screen you see. It runs nothing and is
  not gated.
- **`/dev/url`** deserves the same care from the other side: anything running
  in the guest can open a URL, which means it can launch apps and trigger
  your shortcuts. It cannot do so silently: it works only while iSH-AOK is
  in the foreground, and iOS then switches to whatever it opened, so you
  always see it happen.

## Troubleshooting

**The actions are not in the Shortcuts app.** You need iOS or iPadOS 16 or
later. If you have that, open iSH-AOK once and look again.

**"Running commands from Shortcuts is turned off."** Turn on Settings →
Shortcuts → Allow Shortcuts to Run Commands.

**"The guest system could not boot (error -11)."** The app is open and
asking which saved session to resume. Answer it, then run the shortcut
again.

**"The guest system could not boot" with another number.** The guest
failed to start: no root has been set up yet, or the root is broken. Open
iSH-AOK to see why. A failed boot stays failed until the app process ends,
so after fixing the cause, swipe iSH-AOK away in the app switcher before
trying again.

**"Could not start the command (error -2)."** Neither `/AOK/native/zsh` nor
`/bin/sh` exists in the booted root.

**"Could not start the command as user …"** Open Everything as Default
User is on and the root has no `/bin/su`. Install one, or turn the setting
off.

**`command not found`, or it works in your terminal but not here.** The
command does not get your terminal's environment: `PATH` is the fixed list
above, and `~/.zshrc`, `~/.zprofile` and aliases are not read. Use full
paths, or set what you need in the command itself. As the default user,
`su -` gives you that account's login environment instead.

**A file is created in the wrong place.** The working directory is `/`.
`cd` first, or use absolute paths.

**It always times out.** Either the command takes longer than the timeout
(raise it, up to 120 seconds), or it started a background job that still holds the
output (redirect that job's output, as shown in [Output](#output)).

**"The command was killed by signal 9."** Usually the output reached the
256 KB cap. Send the output to a file and return less of it.

**The result is empty but the command printed something.** The output was
not valid UTF-8. Pipe it through something that produces text, such as
`base64` or `od -c`.

**Colours or escape codes in the result.** The output goes to a pipe, not a
terminal, and `TERM` is `dumb`, so most programs do not colour it. Those that colour regardless need
their own option, such as `--color=never`.

**A long job stops partway when the app is in the background.** iOS limits
how long a background app may run. Open iSH-AOK first, or run the job from
a terminal.

**Open iSH-AOK opens the app but not the destination.** The app is in
recovery mode, set to open Diagnostics at launch, or still waiting for you
to pick a first root. Finish that, then run the shortcut again.
