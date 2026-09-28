# 32. Devices and system integration

A phone can do things a Linux machine cannot: it knows where it is, it has a
camera and a microphone, it has a system-wide pasteboard, it can be automated by
Shortcuts, and on recent hardware it has a language model on the device.

None of that is reachable through a Linux syscall, because Linux has never
needed to reach it. So the question this chapter answers is how an emulated
Linux gets at the machine it is running on — and the answer is almost always
the same shape.

## 32.1 An iOS capability, spelled as a device node

Chapter 18 introduced the family; here is why it is a family.

```sh
echo https://example.com > /dev/url
cat /dev/location
pbpaste < /dev/clipboard
```

The commit that added `/dev/url` states the design rule directly:

> A device rather than a command, matching `/dev/clipboard` and `/dev/location`:
> it composes with redirection and pipes the way a shell expects, and needs no
> binary in the guest filesystem.

Both halves matter. **Composability** means the capability works inside a
pipeline, a shell function, a cron job or a script somebody else wrote —
without a wrapper, a protocol, or a library. And **needing no binary** means it
works in every root: Alpine, Devuan, Arch, i386 or riscv64, freshly installed or
years old, with nothing to install and nothing to keep in step with the app.

A guest-side command would have needed compiling for four architectures,
shipping into every root, and updating whenever the app changed. A device node
needs a line in `fs/dev.c`.

The same reasoning covers the read-only ones — the real-time clock, battery
status, host information via `/proc/ish/host_info` — and they share the honest
limitation: these exist only in the app, because "the command-line build has no
iOS to ask".

## 32.2 Audio

Audio is the exception that proves the rule, because it needs more than a byte
stream: an output engine, format decoding (AVFoundation, Opus, Vorbis), and a
library.

So it exists at two levels. There is a guest-facing device for programs that
want to play sound, and there is a Music applet in Workspace with a library that
lives in `/AOK/persist/music` (Chapter 21) — deliberately, because that
directory survives root switches and app updates, and because it is host-backed
so the app can read it without going through the emulated filesystem at all.

That is the second time in this book that `/AOK/persist` is chosen for its
*host-backed* property rather than its persistence: Chapter 21 noted the same
reasoning for the LLM chat log. When the app itself is a consumer of a file, the
fakefs translation is a cost with no benefit.

## 32.3 Shortcuts, and four traps

Apple Shortcuts is the phone's own automation system, and the integration makes
the guest a first-class participant in it.

**"Run Command"** executes a command in the guest and returns its output to the
shortcut — headlessly, through `run_guest_command_capture_shell()` (Chapter 15's
`kernel/init.c` machinery) running `/AOK/native/zsh -c`, so **the app never has
to come to the foreground**. **"Open iSH-AOK"** destinations with Siri phrases
cover the cases where it does.

It is gated behind a preference, `shortcuts_run_commands` in
`/proc/ish/defaults`, because a shortcut that can run arbitrary guest commands is
a capability a user should switch on deliberately.

Combined with `/dev/url` (Section 32.1), the loop closes in both directions: a
shortcut can run a guest command, and a guest command can run a shortcut. The
commit that added the return path names that as the point of it — anything
automated on the phone becomes reachable from a shell script.

Getting there cost four traps, and they are worth recording because each is
invisible until it has wasted a day.

**Unsigned simulator builds cannot exercise the feature.** `linkd` rejects an
ad-hoc bundle, so App Shortcut tiles and Siri phrases fail to fetch
("Couldn't find AppShortcutsProvider") and `AppEnum` parameter values **silently
resolve to their default**. String and integer parameters work, and intents
execute normally — so the feature looks fine in the simulator while three of its
surfaces are inert. And the obvious workaround makes it worse: manually
code-signing with a development certificate made the app unlaunchable. Those
surfaces can only be verified on a properly signed device build.

**Xcode incremental builds clobber the intents metadata.** The metadata
processor reruns and rewrites the bundle directory, while the training step
considers itself up to date — leaving its outputs missing. The workaround is to
run the training processor by hand *after* Xcode signs, because re-signing by
hand breaks launch.

**Scene activation is silently dropped on iPhone.**
`requestSceneSessionActivation(nil, userActivity:)` delivers the activity only
when it *creates* a scene, which is an iPad behaviour. On iPhone it foregrounds
the app and drops the activity — and `-scene:continueUserActivity:` is not called
either, which was established by breadcrumb (Chapter 28's diagnostics earning
their place). The fix routes activities to a connected scene's window directly.

**And one that was not an app bug at all.**

> **The bug that taught us this**
>
> A task whose first `exec` is a native program has `mm->exefile == NULL`,
> because no guest image is ever loaded (Chapter 22).
>
> Anything that dereferences `exefile` without a guard therefore dies on the
> first `fork` — and `mm_copy` did.
>
> That is a kernel bug, surfaced by an app feature, because "the first program a
> task ever runs is native" is a state no ordinary boot produces. Shortcuts runs
> `/AOK/native/zsh` as a fresh task's first image, and nothing had ever done that
> before.
>
> The rule left behind: grep for new `exefile` uses when touching exec or fork.

That last one is the most transferable thing in the chapter. An integration is
not only a feature; it is a **new combination of existing states**, and the
combinations are where the latent bugs are.

## 32.4 A language model with a shell

The LLM chat client talks to an OpenAI-compatible API, to Google Gemini, or — on
iOS 26 and later — to Apple's on-device Foundation Models. It is off by default
and appears in the terminal's session menu and in the Workspace dock once
enabled.

The bridging is small and instructive. `AOKFoundationModelsBridge.swift` has to
flatten Apple's own availability type, because `SystemLanguageModel.Availability.UnavailableReason`
has associated values and is not representable as an `@objc` enum. And the
`run_shell` tool cannot execute anything itself:

> Executing a command means reaching back into the (Objective-C) guest-shell +
> confirmation-dialog machinery … which this Swift file has no direct access to
> — so the actual work is delegated through `AOKFoundationModelsBridge`'s
> `shellCommandHandler`.

The interesting part is the **security posture**, which is worth reading as a
model for this kind of feature.

By default, every command is confirmed before it runs. Output is capped (64 KB),
runtime is capped (30 seconds), and a single reply is capped at a number of tool
rounds (20) — all three adjustable. There are escape hatches: "Run, don't ask
again this reply", and an "Allow All" that lasts for the chat. Auto-run re-arms
when the chat is cleared.

And then the documentation says this, in its own voice, to its own users:

> Worth understanding before you use it: content the model fetches — a web page,
> a file — can instruct it to run destructive commands or read private data, and
> in auto-run nothing stops that but the model itself.

That is a product telling its users about prompt injection, in the section that
explains how to disable the confirmation that prevents it. It is unusually
honest, it is correct, and it belongs in this book because it is the same
instinct as everything in Chapter 40: describe the actual state of affairs,
including the parts that make your feature look worse.

## 32.5 The Wayland desktop

The Wayland applet is the one window in the app whose contents are drawn by
guest programs rather than by UIKit: a real wlroots compositor, real Linux
clients, a real session. It is also the clearest case of this chapter's pattern,
because the design decision that made it tractable is the same one — keep every
hard problem on the Linux side, where the software already exists, and give the
app as little to do as possible. The app has no compositor and no window
management. The design note called it "a dumb pixel pipe", and in 557 it still
is one; what changed is what the pipe is made of.

### Setting it up

Two scripts do everything, and the division between them is the division
between installing and running.

`setup-wayland.sh` runs once, as root, and installs the stack from the
distribution: `labwc` (the default compositor, a floating desktop with a
right-click menu), `sway` as a tiling alternative, `foot` as the first terminal,
`wofi`, `wayvnc`, `waybar` with the icon font it needs, and `dbus-daemon`.
Three optional sets follow from `setup-wayland-extras.sh` — games, desktop
tools, and Xwayland for X11-only programs — and `setup-gpu.sh` adds the Vulkan
driver, zink and a checker (below). It has been run on Devuan and Alpine, on
amd64 and arm64 guests; Arch resolves the same package names and nobody has run
a session on it, and the user's guide says exactly that.

`start-wayland.sh` is not something the user normally runs; the applet does. It
runs as the session leader of a pseudo-terminal the applet owns — the same
mechanism a terminal window uses, without the terminal — so closing the applet
hangs up the pty and the script's `SIGHUP` trap tears the session down. It
starts the compositor, the first terminal, `wayvnc` on port 5901, and a session
D-Bus of its own, then tells the app it is ready by writing the port to
`/tmp/ish-display.ready`. On failure it writes `.error` beside that file with
the reason, so the applet can show *why* — the compositor crashed, `foot` died,
`wayvnc` never listened — instead of timing out generically 45 seconds later.
The script's stdout is useless for that: it travels through a terminal
emulator's escape-sequence parser, not a pipe.

Most of what the script does beyond that is the accumulated knowledge of what
goes wrong on the first run of a desktop in a root nobody configured for one:

- **A session bus.** These roots have no systemd user session, so GLib and Qt
  programs found no bus and `waybar` refused to start ("Cannot autolaunch D-Bus
  without X11 `$DISPLAY`"), which was also Falkon's report in #485. The
  desktop now brings its own, and the fix surfaced a kernel bug on the way: a
  GLib client authenticates from a worker thread, and AOK checked
  `SCM_CREDENTIALS` against the *thread's* id where Linux wants the process's.
- **Configuration that is the user's.** `labwc`'s menu and key bindings and
  `waybar`'s config are written only when the user has none, and a file the
  user has changed is never replaced. One still exactly as an earlier AOK wrote
  it is brought up to date, which is how an existing desktop gets new menu
  entries.
- **One desktop at a time.** Sessions share the VNC port, the ready file and a
  cache directory, and an early version of the script began by sweeping away
  anything named `labwc`, `foot` or `wayvnc` — so a second start ended the
  desktop the user was looking at. It now finds a live session through
  `/proc` (not a lock file: `/tmp` is wiped part-way through boot) and refuses,
  with the running session's pid in the message.
- **A second session after the first.** wlroots creates `/tmp/.X11-unix` itself
  with the session's umask, so a root desktop left it `0755`; once `bind()`
  checked directory permissions as Linux does, the next non-root desktop could
  not create its X display and exited. The script makes the directory `1777`.
  And guest pids restart at 1 on every boot, so a stale `/tmp/.X0-lock` can
  name the new compositor's own pid: anything that trusts a lock file by pid
  needs a freshness check as well.

### Two ways a frame reaches the screen

**Through VNC**, which is how the applet was built. It began on 2026-07-12 as
noVNC in a web view and was replaced the next day by a native RFB client,
`DisplayRFBClient`, drawing with Metal. Since 555 the desktop is the size of
the window showing it — the client asks with RFB `SetDesktopSize` whenever that
window changes — except with a `neatvnc` older than 0.9.2, which crashes when
the desktop shrinks under a connected client, so the script makes `wayvnc`
refuse and the desktop keeps 1280x720, scaled.

The VNC path has one cost that no tuning removes: `wayvnc` captures and encodes
the whole framebuffer in software, *inside the emulator*. At one pixel per point
that is affordable. An iPad at 2x is 1668x2420 at four bytes a pixel, which at
30 frames a second asks an emulated guest for about 480 MB/s. It was reported as
"terminal updates are glacially slow" while menus stayed responsive — the
signature of a capture competing with the one client that redraws all the
time — and the app answers by lowering `wayvnc`'s frame rate as the resolution
goes up. Profiled on an M4 iPad during vkcube, `wayvnc`'s threads were 44% of
the app's CPU: the display link, not the rendering, had become the cost.

**Through `wl-present`**, which is 557's answer to that. It is a native program
(Part V) that connects to the compositor as an ordinary Wayland client, asks it
to copy each damaged frame into DRM dumb buffers on the GPU render node, and
hands that shared memory to the app through the kernel. The app wraps it as a
Metal buffer and blits the damaged rectangle. No pixel passes through emulated
code. It carries the other direction too, from a kernel queue the app writes:
pointer and keyboard through the compositor's virtual devices, the clipboard
both ways, and the desktop's size. With all of that off its hands `wayvnc` has
nothing left to do, so once the app is showing frames `wl-present` has it
detached, and re-attached when `wl-present` ends however it ends; the app's VNC
connection stays up throughout as the fallback.

Measured on the M4 with vkcube running, in device-wide busy ticks a second:
38–39 over VNC with frames capped at 30 a second, 35–36 with `wl-present` and
`wayvnc` detached, with frames up to 60 a second and `wayvnc` at 0.2% of the
CPU. `ISH_DISPLAY_DIRECT=0` puts everything back on VNC.

### The GPU underneath

`wl-present` needs the compositor to be drawing on the GPU, and since 557 it
does by default wherever `setup-gpu.sh` has installed the pieces. The render
node, `/dev/dri/renderD128`, is this chapter's pattern again: an iOS
capability — Metal, the only way an app reaches the GPU — spelled as the Linux
device node stock Mesa expects, with Vulkan replayed onto Metal through Venus
and MoltenVK inside the kernel. Chapter 41 lists its edges and
`docs/roadmap.md` has the gate it passed.

Two choices in the script are worth knowing. The compositor loads zink only
because a private driver config names it for `labwc` and `sway`: zink on this
GPU offers OpenGL 2.1 and GLES 2.0 where software rendering offers 4.5, so
every other GL program stays in software unless run through `gpu-run` — a
program that needs more than 2.1 then runs slowly instead of failing. And if the
compositor's output does not come up on the GPU, the script falls back to
software rendering — wlroots' pixman renderer, with Chapter 33's accelerator
underneath — and says so. `ISH_DISPLAY_GPU=0` starts there.

### What the desktop found, and what is still open

A desktop is a conformance test nobody wrote. Besides the credential and
`bind()` bugs above, it found a signal and poll deadlock that `labwc`'s menu
triggered by forking and exiting (it looked like an unreliable menu, and for a
while `sway` was the default because of it), and a keyboard trap in `wayvnc`:
sent Alt and Shift and a lowercase letter, it looks up the key for the letter's
own level, lifts the held modifiers to type it, and so `labwc`'s Alt+Shift
bindings had probably never fired from the app. The client now sends the
uppercase letter while Shift is held; the same trap still lifts Ctrl from a
Ctrl with `+`.

What is open is recorded in `docs/TODO.md`: saving a session with the applet
open is slow and has not been measured; a restore comes back in the mode the
Settings preference names rather than the one it was saved in; and the
standalone Wayland window has no Save Session action of its own. The newest
report is a list of programs that misbehave on the desktop (#620), which is the
shape this work has always had — the feature exists, and each program is a
conformance question.

## 32.6 What every integration owes

Each feature in this chapter can be unavailable, and for a different reason.

The device nodes do not exist in the command-line build, because there is no iOS
underneath it. Foundation Models needs a recent OS, eligible hardware and Apple
Intelligence enabled — five distinct unavailability reasons, which is why the
bridge flattens them into an enum rather than a boolean. The File Provider is
switched off on Macs entirely (Chapter 31). Shortcuts' enum parameters do not
work in an unsigned build.

The obligation that comes with that is the one this book keeps returning to:
**say so.** `/proc/ish/roots` reports `job state=unavailable` on the CLI
(Chapter 30). The Files documentation explains that a missing feature is better
than a crash you cannot act on. The availability enum carries the reason rather
than just the fact.

An integration that is present but inert is worse than one that is absent,
because absence is diagnosable and inertness is not. Chapter 40 states the
general rule; this chapter is where it costs the most, because these are the
features whose failures happen on somebody else's device, in a configuration the
developer does not have.

---

*Anchors:* [app/URLDevice.m](../../app/URLDevice.m),
[app/PasteboardDevice.m](../../app/PasteboardDevice.m),
[app/LocationDevice.m](../../app/LocationDevice.m),
[app/RTCDevice.m](../../app/RTCDevice.m),
[kernel/BatteryStatus.m](../../kernel/BatteryStatus.m),
[kernel/hostinfo.m](../../kernel/hostinfo.m),
[app/AudioPlayerEngine.m](../../app/AudioPlayerEngine.m),
[app/AudioDevice.m](../../app/AudioDevice.m),
[app/ISHAppShortcuts.swift](../../app/ISHAppShortcuts.swift),
[app/ISHRunCommandIntent.swift](../../app/ISHRunCommandIntent.swift),
[app/GuestCommandRunner.m](../../app/GuestCommandRunner.m),
[app/AOKFoundationModelsBridge.swift](../../app/AOKFoundationModelsBridge.swift),
[app/DisplayRFBClient.m](../../app/DisplayRFBClient.m),
[app/DisplayViewController.m](../../app/DisplayViewController.m),
[kernel/native_wlpresent.c](../../kernel/native_wlpresent.c),
[fs/virtgpu.c](../../fs/virtgpu.c),
[opt/AOK/tools/setup-wayland.sh](../../opt/AOK/tools/setup-wayland.sh),
[opt/AOK/tools/start-wayland.sh](../../opt/AOK/tools/start-wayland.sh),
[opt/AOK/tools/setup-gpu.sh](../../opt/AOK/tools/setup-gpu.sh),
[opt/AOK/docs/workspace.md](../../opt/AOK/docs/workspace.md) ("The Wayland applet"),
[kernel/init.h](../../kernel/init.h) (`run_guest_command_capture_shell`),
[opt/AOK/docs/shortcuts.md](../../opt/AOK/docs/shortcuts.md),
[opt/AOK/docs/llm-chat.md](../../opt/AOK/docs/llm-chat.md).

*Story:* `mm_copy` dereferencing a null `exefile` — a latent kernel bug that
nothing had ever reached, until Shortcuts made a native program the first image
a task ever ran.
