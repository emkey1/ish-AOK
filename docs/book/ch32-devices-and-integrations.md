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

## 32.5 Pixels

`DisplayRFBClient` and `DisplayRFBView`, with Metal shaders, are a VNC client
inside the app. There is no compositor and no window management on the app's
side — the design note for the Wayland work is explicit that the app stays "a
dumb pixel pipe".

That deliberate minimalism is what makes the plan in Chapter 42 tractable: a
guest-side headless `wlroots` compositor plus `wayvnc`, and an in-app viewer
that only has to display frames and forward input. Every hard problem stays on
the Linux side, where the software already exists.

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
[kernel/init.h](../../kernel/init.h) (`run_guest_command_capture_shell`),
[opt/AOK/docs/shortcuts.md](../../opt/AOK/docs/shortcuts.md),
[opt/AOK/docs/llm-chat.md](../../opt/AOK/docs/llm-chat.md).

*Story:* `mm_copy` dereferencing a null `exefile` — a latent kernel bug that
nothing had ever reached, until Shortcuts made a native program the first image
a task ever ran.
