# 31. Files, Workspace, and the app-side tools

The chapters so far in Part VI have been about making a terminal work. This one
is about everything the project built *around* the terminal because a phone is
not a laptop: a way to get files in and out, a windowing environment, viewers
for the things a terminal cannot show, and a process viewer that understands
what this system actually is.

The unifying question is a design one, and it comes up in every section: when
the guest can already do something with a command, what justifies building a
native surface for it?

## 31.1 Files: the guest filesystem in the system picker

The File Provider extension exposes installed roots through Apple's Files
framework, so guest files appear as a location in the Files app and in every
other app's document picker. No `scp`, no export step.

It is a separate iOS *extension target*, which means a separate process — and
that is why Chapter 17's `0xdead10cc` fix had two halves. The extension opens
the same fakefs databases the app does, and iOS killed it inside `sqlite3_close`
for the same reason it killed the app mid-transaction. Two processes, one
database, one platform rule about locks and suspension.

Two details are worth recording because both are choices rather than accidents.

**The location is called "iSH"**, not "iSH-AOK" — the extension's display name
was never renamed, and the documentation says so plainly rather than leaving
somebody to wonder whether they are looking at the right app.

**It is switched off on Macs**, deliberately, and the reasoning is a small model
of how to handle a platform that will not cooperate:

> The extension is built on Apple's `NSFileProviderExtension`, which their own
> SDK marks unavailable on macOS — a Mac hosts only the newer replicated File
> Provider API. Left enabled, macOS loads the extension, finds one it cannot
> host, and kills it before any iSH-AOK code runs. So the app does not register
> a provider there at all; **a missing feature is better than a crash you cannot
> act on**.

A crash before any of your own code runs is the worst possible failure to
diagnose from a report, because nothing in your logs happened. Declining to
register is the honest version.

## 31.2 One queue was the wrong number

Underneath the file surfaces sits `GuestFileBridge`, the app's route into the
guest filesystem — and its design note is the best short piece of
performance-engineering writing in the tree.

It began with one serial queue:

```objc
_ioQueue = dispatch_queue_create("app.ish.guestfilebridge.io", DISPATCH_QUEUE_SERIAL);
```

Everything went on it: directory listings, `stat`, `mkdir`, rename, delete,
whole-file reads and writes, chunked extraction to a temp file, and cross-backend
copies. So any long operation blocked every short one behind it.

The diagnosis names the failure class precisely:

> That is not a throughput problem, it is a **latency-class problem**. A 300 MB
> extraction is *supposed* to take a minute. What is not acceptable is that a
> `readdir` of twelve entries queued behind it also takes a minute.

And the file manager made it maximally visible, because `-setLoading:` disables
user interaction on the whole table for the duration — so the folder was not
merely stale, it was untappable. That is the mechanism behind user reports of
the file manager going "completely unresponsive trying to do anything with the
folder, including just viewing it".

What makes the note exemplary is the two fixes it rules out before proposing
one.

**Not "make it concurrent."** Chapter 17 measured fakefs's metadata mutex at 78%
duty under a *single* thread, and parallel metadata work measured *slower*. A
concurrent queue sized to the core count "would make every number worse and fix
nothing, because the listing would still be sharing a lock with the copy".

**Not "split the queue and hope."** Callers do write-then-reload — create a
folder, rename, duplicate, delete, then reload — and expect to see the result. A
second queue that let a reload overtake the write it should follow "would make
the user's new file intermittently not appear", which is a worse bug than
slowness because it is unreproducible.

The answer is **two serial lanes, never a concurrent queue**: an interactive
lane for operations whose cost is bounded (a pid lookup, a `stat`, a listing)
and a bulk lane for everything else, with the lane chosen at enqueue time from
information available without touching the VFS, and with the orderings that are
load-bearing written down explicitly.

The trade is stated rather than hidden: "the bulk transfer gets slower because
the listing now interleaves with it, which is precisely the trade being bought."

## 31.3 Workspace

Workspace is a windowing environment inside the app: a dock, multiple virtual
desktops, and draggable, resizable applet windows, with layouts that can be
saved and recalled.

The architectural fact that matters is what it is *not*:

> Workspace windows are built entirely in native UIKit — it is **not** a Wayland
> or X11 guest display. Only actual terminal sessions inside it run guest Linux
> processes; the window chrome, dock, and most applets are pure native code.

Which is also why it works on an iPhone: it does not depend on iOS scenes or
Stage Manager, so multi-window is available on a device the platform does not
give multiple windows to.

The applet list is longer than a book chapter should enumerate — a file manager,
MotePad, a markdown viewer, an image viewer, a video player, a music player, a
browser, clock and monitor and network and log panels, storage, the Filesystems
screen of Chapter 30, settings, diagnostics, an LLM chat (Chapter 32), and a
Wayland display (Chapter 42).

The design question from the top of this chapter applies hardest here, and
`docs/native_workspace_design.md` answers it directly: the goal "is not to build
a Linux desktop stack", but to let the guest keep providing shell and process
semantics while the visible workspace, layout and widgets are owned by the app.
A clock applet does not need a Linux process behind it, and one drawn in UIKit
costs nothing to run and looks like the platform.

## 31.4 MotePad, and the markdown renderer's two jobs

`MotePad` is the text editor applet, with a native counterpart compiled in as a
native program (Chapter 25) so the same editor is available from a shell. Two
front ends, one idea, which is the pattern Chapters 28 and 30 keep arriving at.

The **markdown renderer** is more interesting, because it does two jobs that
turn out to be in tension. It renders local `.md` files — the `/AOK/docs` set of
Chapter 21, and whatever the user has — and it also renders web pages converted
to markdown-ish text, which is what makes the browser applet and the `md` tool
useful.

> **The bug that taught us this**
>
> The renderer carries a pile of web-scrape cleanup: dropping CSS and script
> lines, reassembling fragmented links, detecting news-page metadata. All of it
> was running on **local `.md` files too**.
>
> Which is how an ordinary sentence containing two commas and a filename was
> **deleted** as a CSS selector list.
>
> The fix gates every scrape heuristic on a flag that is true only while
> rendering converted HTML. The operational tell is worth keeping: if a document
> looks *edited* rather than merely mis-formatted, suspect that gate first.

A second story from the same file is a good example of fixing a bug at the level
where the information exists. Words were running together, and a camelCase
splitter had been added to compensate — which is why "iSH-AOK" rendered as
"i SH-AOK".

The real cause was that the HTML converter dropped tags without putting
whitespace where they had been. So the splitter went, and the converter now
classifies tags as inline, spaced, line or block — "so the word boundary comes
from the element, which is the only place that information exists".

Its known limit is stated too: CSS can make an inline element behave as a block
one, and the converter does not read CSS, so adjacent `<span>`s styled as blocks
still run together. The note calls that "the correct trade — wrong about
navigation furniture, not about `macOS` in every document", which is exactly the
right way to choose between two imperfect behaviours.

## 31.5 `ktop`, and the column no other tool has

`ktop` is an htop-style process viewer, written from scratch in C99 against
`/proc`, with no ncurses and no procps. It lives in `/AOK/tools`, which means it
travels with the app rather than with any root (Chapter 21).

Its reason to exist is one column:

> iSH-AOK can run i386, amd64, arm64, and riscv64 binaries side by side in one
> booted session … Stock `top`/`htop` have no way to show that architecture mix.
> `ktop`'s one real differentiator is an **ARCH** column, read directly from each
> process's ELF header via `/proc/<pid>/exe`.

That column is only meaningful because of two facts established much earlier in
this book: a task's guest ABI is a field on the task (Chapter 7), and there are
no PID namespaces, so one process viewer sees every process in every root
(Chapter 21). A tool that shows what this system uniquely is could not exist on
a system that had isolation.

Two practical notes come with it. `/AOK` is read-only, so `ktop` cannot be built
in place — `build.sh` copies the source out first, for whichever guest
architecture you are on. And its prebuilt binaries were, for a while, quietly
stale: Chapter 35 tells that story, because the CI job that produces them had
been failing since before build 547 and nothing said so.

## 31.6 What justifies a native surface

Back to the question this chapter opened with. Every applet here duplicates
something the guest can already do — `ls`, `vi`, `top`, `cat`, a pager.

Three answers emerged, and they are worth separating because only the first is
about speed:

**Some things the terminal genuinely cannot do.** Display an image. Play a
video. Present a file to another app's document picker. A terminal is a
character grid, and no amount of emulator work changes that.

**Some things the platform expects to own.** iOS users expect files to appear in
Files, media to play with system controls, and text to be selectable and
shareable. Meeting those expectations means using the platform's own components,
not reimplementing them behind a terminal.

**And some things are simply cheaper native.** A clock applet drawn in UIKit
costs nothing; a clock in a guest process costs a task, a translation cache and
a wake per second. Chapter 27's rule about when *not* to go native has a mirror
image here: when the work has no guest semantics to preserve, the guest is the
wrong place for it.

What holds it together is that none of them replace the terminal. The dock's
first tile is Terminal, `/AOK/tools` ships command-line equivalents of the app's
own tools, and every applet that touches the filesystem goes through the same
bridge, with the same lanes, to the same fakefs the shell sees. There is one
system underneath, and the surfaces are views onto it.

---

*Anchors:* [app/FileProvider/](../../app/FileProvider),
[app/GuestFileBridge.m](../../app/GuestFileBridge.m),
[app/WorkspaceViewController.m](../../app/WorkspaceViewController.m),
[app/WorkspaceFileManager.m](../../app/WorkspaceFileManager.m),
[app/MarkdownRenderer.m](../../app/MarkdownRenderer.m),
[app/MotePadDocumentStore.m](../../app/MotePadDocumentStore.m),
[app/ShellFileBrowser.m](../../app/ShellFileBrowser.m),
[opt/AOK/tools/ktop/ktop.c](../../opt/AOK/tools/ktop/ktop.c),
[docs/guest_file_bridge_lanes.md](../../docs/guest_file_bridge_lanes.md),
[docs/workspace_file_manager_plan.md](../../docs/workspace_file_manager_plan.md),
[docs/native_workspace_design.md](../../docs/native_workspace_design.md),
[opt/AOK/docs/workspace.md](../../opt/AOK/docs/workspace.md),
[opt/AOK/docs/files-app-integration.md](../../opt/AOK/docs/files-app-integration.md),
[opt/AOK/docs/ktop.md](../../opt/AOK/docs/ktop.md),
[opt/AOK/docs/md.md](../../opt/AOK/docs/md.md).

*Story:* a sentence with two commas and a filename in it being deleted from a
local document — because the markdown renderer's web-scrape cleanup, written to
strip CSS selector lists out of converted HTML, was running on every file.
