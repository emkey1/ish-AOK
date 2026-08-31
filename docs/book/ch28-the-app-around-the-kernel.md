# 28. The iOS app around the kernel

Everything in Parts II through V happens inside a process that iOS can suspend
at any moment, kill for using too much memory, or terminate for holding a lock
at the wrong time. Part VI is about that process — and this chapter is about the
part of it whose entire job is keeping the kernel alive.

## 28.1 Boot

`app/main.m` still carries the header comment it was created with:

```objc
//
//  main.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//
```

and it does two things:

```objc
static dispatch_once_t onceToken;
dispatch_once(&onceToken, ^{ run_at_boot(); });
retVal = UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
```

`run_at_boot()` is the emulator's own initialization, before UIKit exists.
`UIApplicationMain` then never returns, and the rest of the system is
callbacks: `AppDelegate` for process-level events, `SceneDelegate` for each
window, `TerminalViewController` for the thing the user actually looks at.

Chapter 1's claim that the kernel is a library is most visible here. There is no
"start the emulator" step and no emulator thread. A guest task is a host thread
created by `task_start`, `current` is a thread-local, and the UI is simply
another thread in the same process that happens to be the one UIKit calls.

## 28.2 Nobody tells you when you are about to be suspended

This is the central problem of the chapter, and the solution is the neatest
trick in the app.

iOS notifies an application when it is *backgrounded*. It does not notify it
when it is about to be *suspended* — the point at which threads stop running and
the process becomes a frozen image. Everything in Chapter 17 (fakefs must hold
no lock) and Chapter 19 (listening sockets must be recorded) has to happen
before that moment, and the moment is not announced.

The workaround uses a different API for a purpose it was not designed for:

> Backgrounding does not tell us when suspension will land, so take an
> assertion: its expiration handler is iOS telling us the extra time is up,
> which is the closest thing to an "about to be suspended" signal, and that is
> where we get the filesystem to a lock-free state.

A background-task assertion asks the system for extra running time. Its
expiration handler fires when that time runs out — which is precisely when
suspension becomes imminent. So the app takes an assertion it does not need, in
order to be told when it is about to lose it.

## 28.3 Three refinements, three real failures

The naive version of that is four lines. The shipped version has three
qualifications, and each one is a bug that reached a device.

**Arm once, however many windows there are.** On iPad the app can have several
scenes, and each reports its own transition — so with two windows, both observe
"everything is backgrounded" and both call in. Without a guard, each takes an
assertion and only the last is tracked, leaking the earlier ones. It was
"observed on device as the whole background/quiesce sequence logged twice",
which is the kind of symptom that only shows up in a log somebody bothered to
read.

**Do not quiesce if something is genuinely keeping the app alive.** With
background location updates running, the app really does keep executing and is
*not* about to be suspended. Freezing guest filesystem I/O there would stall
long-running background work for no reason — and the assertion expires on its
own schedule regardless. So the handler checks first.

**Never hold the gate open indefinitely.** This one deserves quoting in full,
because the failure it describes is the most insidious kind:

> If iOS really does suspend us, this block cannot run until we resume, so the
> filesystem stays quiesced across the suspension — which is the whole point,
> and nothing is executing meanwhile anyway. But if iOS does NOT suspend us …
> then waiting for `-sceneWillEnterForeground:` to lift the gate freezes the
> guest filesystem for as long as the app sits in the background. That is not a
> subtle degradation: **an incoming ssh session gets accepted and then blocks
> before it can reach a shell**, because fork, exec, PAM and the home directory
> all need a fakefs transaction, so the device silently stops serving while
> looking perfectly healthy.

Accepted, then hung. The TCP connection succeeds, the banner may even arrive,
and then nothing — while the app is running, the process is alive, and every
external check says the service is up. A `dispatch_after` lifts the gate after a
bounded interval, precisely so that a decision made for suspension cannot
outlive the suspension that justified it.

The general shape: **a safety measure taken in anticipation of an event needs a
plan for the event not happening.**

## 28.4 Memory, and reading a crash report correctly

Chapter 13 covered the mechanism — `host_mem_headroom_low()` making guest `mmap`
and `brk` return `ENOMEM` while the device still has memory, so that UIKit and
libobjc keep allocating and the app is not jetsam-killed with a null dereference.

What belongs here is how the resulting failures are *read*, because they arrive
as crash reports from strangers' devices.

Chapter 14's `pidfd_epoll_deadlock` investigation is the worked example. The
report said "exits 137". `cli_halt` maps a signalled init to `128 + signo`
(Chapter 1), so 137 reads as "the guest was `SIGKILL`ed" — and the *host* process
had been killed instead. Fourteen crash reports, byte-for-byte identical stacks,
and the guest's exit status was a coincidence of arithmetic.

The rule that came out of it: **exit 137 may be the host, not the guest.** Check
`~/Library/Logs/DiagnosticReports` (or the device's equivalent) before believing
a guest-side story, and read the `asi` field, which names the assertion that
failed.

## 28.5 Diagnostics as a subsystem

`AppDelegate.m` is 3,757 lines, and a striking fraction of it is not the app at
all — it is `ISHDiagnosticsStore`, which exists because an emulator shipped to
other people has to be able to explain its own death.

It records:

- **Breadcrumbs** — named events with details, kept in a ring.
- **A launch journal** with stages, so a crash during startup names the stage it
  reached rather than a stack in `dyld`.
- **Guest fatal events and recent guest exits**, so a report can distinguish "a
  guest process died" from "the app died".
- **MetricKit payloads**, persisted when iOS delivers them, which is how crashes
  and hangs on other people's devices reach the project at all.
- **A diagnostics report and an export bundle**, so a user can send all of it in
  one action.

That last one matters more than it sounds. The alternative to an export bundle
is asking a user to reproduce a crash while a developer watches, on a device the
developer does not have, running a rootfs the developer did not build.

## 28.6 Preferences have two front doors

`UserPreferences` is the app's settings store, observed with KVO so that a
change propagates immediately: the theme, the font, the launch command, and the
toggles that reach into the emulator — HLE (Chapter 8), the crypto and pixman
accelerators (Chapter 33).

The second front door is `/proc/ish/defaults` (Chapter 18): the same store,
one file per preference, readable by anyone and writable by root, with the same
validation the Settings screen uses.

Two surfaces onto one store is a small thing that changes what the product is.
A setting reachable only by tapping cannot be scripted, cannot be set from an
`ssh` session, and cannot be part of a provisioning script — and this is an
application whose users are people who automate things.

## 28.7 Containers, extensions, and a shared database

The app's files live in two places, and the distinction matters.

The **app sandbox** holds installed roots. The **App Group container** holds
`/AOK/persist` (Chapter 21), which is why it survives reinstalling the app.

The App Group is shared, and it is shared with another *process*: the File
Provider extension (Chapter 31), which iOS runs separately and which opens the
same fakefs databases. That is why the `0xdead10cc` work had two halves — one in
the app, which was being killed mid-transaction, and one in the extension, which
was being killed inside `sqlite3_close`. Two processes, one database, one
platform rule about locks and suspension.

It is also a source of a distinctive crash: the app terminating on launch when
the App Group container is missing, because a container that should always exist
is exactly the thing nobody null-checks.

## 28.8 Scenes: several windows, one kernel

On iPad the app supports multiple windows, and each is a `UIScene` with its own
`SceneDelegate`, its own terminal view, and its own guest session.

There is still one kernel, one process table and one filesystem underneath all
of them — which is the same "one true system" property Chapter 21 described,
arriving in the UI. Two windows are two terminals on the same machine, not two
machines.

The lifecycle consequences are real and this chapter has already met one of
them: the suspension guard's arm-once rule exists because scene transitions are
per-scene and suspension is per-process. Chapter 32's Shortcuts integration met
another, where an action that needs a scene had to cope with activation being
dropped.

## 28.9 The blast radius

One rule runs through the whole app layer and is worth stating explicitly, since
this is the chapter where it is decided.

**There is no isolation, so an assertion failure is a decision to destroy the
user's session.**

In a normal kernel, an assert in a filesystem is a panic — bad, but bounded, and
the machine reboots. Here the "machine" is a terminal application, and an abort
takes the shell, the editor, the `ssh` session and anything unsaved with it.

The consequences appear throughout this book and they all come from here:
`get_mem_usage()` degrading to best-effort values rather than asserting when
Mach calls fail (Chapter 1); tmpfs's aborts removed (Chapter 18); the native
stack guard (Chapter 27); the deferred fatal signal inside stdio (Chapter 27);
the memory-headroom refusal (Chapter 13). Every one of them trades a clean
diagnostic for a degraded but surviving session.

That is not the trade a kernel usually makes. It is the right one when the
kernel is an app, and the difference between an emulator that people use and one
they stop trusting is mostly a list of places where somebody chose to keep
running.

---

*Anchors:* [app/main.m](../../app/main.m), [app/AppDelegate.m](../../app/AppDelegate.m)
(`ISHSuspendGuardEnterBackground`, `ISHDiagnosticsStore`),
[app/SceneDelegate.m](../../app/SceneDelegate.m),
[app/UserPreferences.m](../../app/UserPreferences.m),
[app/AppGroup.m](../../app/AppGroup.m),
[fs/fake-db.h](../../fs/fake-db.h) (the quiesce contract),
[platform/platform.h](../../platform/platform.h) (`host_mem_headroom_low`),
[fs/proc/ish.c](../../fs/proc/ish.c), [kernel/log.c](../../kernel/log.c).

*Story:* an `ssh` session accepted and then hanging before it reached a shell —
because the filesystem had been quiesced for a suspension that never came, and
every path to a login needs a fakefs transaction.
