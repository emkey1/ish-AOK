# 42. Where it could go

The last chapter of a book about a working system is usually a wishlist. This
one is deliberately not, because the project's own notes distinguish sharply
between things that have been *proven possible*, things that are *scheduled
behind a gate*, and things that are *thought experiments* — and that distinction
is the useful part.

## 42.1 Wayland, and why it became possible

The most advanced unfinished work is a graphical desktop, and the interesting
thing is that almost none of it is graphics.

The architecture keeps every hard problem on the Linux side: a **headless
`wlroots` compositor plus `wayvnc` running in the guest**, displayed by a VNC
client in a Workspace applet, connected over localhost. No compositor is
written; the app is, in the plan's own words, "a dumb pixel pipe" (Chapter 32).

Two tiers, and the split is honest about where the work is:

- **Tier 1** — the Wayland stack runs headless in the guest and is viewable from
  *any* VNC client. Zero or near-zero app changes; the work is emulator
  conformance fixes as they surface.
- **Tier 2** — a Workspace "Display" applet embeds the client so the whole thing
  lives in the app.

What makes this feasible *now* rather than in principle is a list of things that
were fixed for other reasons, and it reads as a summary of Parts III and IV:

- `wl_shm` needs `memfd` plus `MAP_SHARED` mmap — the tmpfs and memfd
  host-file backing of Chapters 5 and 13.
- File-descriptor passing over Unix sockets via `SCM_RIGHTS` — Chapter 19.
- `epoll`, `timerfd` and `signalfd`, regression-tested — Chapter 14.
- pixman's hot paths running host-natively — Chapter 33.

None of that was done for Wayland. Wayland became reachable because a general
kernel got more correct.

The plan's Phase 0 is also a good model for scheduling risky work: everything is
behind a gate that runs on the CLI harness with a known-good rootfs, and the
expected schedule owner is named as *the bug-hunt loop* rather than the feature
work. The likely suspects are listed in advance — `memfd`/shm coherence between
**unrelated** processes (fork-based coherence is tested; pass-an-fd-then-mmap is
not), the XKB keymap fd, epoll edge-trigger corners in wlroots' event loop,
`clock_nanosleep` precision for frame timing, inotify — with a standing
instruction that each fix lands with a focused guest-side regression test, and a
policy note that says *implement rather than stub unless genuinely harmless*.

That is what planning looks like when the unknown is conformance rather than
design.

## 42.2 The display that was built and rejected

External display and AirPlay mirroring exist on a branch and are not merged,
because the maintainer judged the work flawed (Chapter 41). It stays fenced
rather than merged or deleted.

Worth restating here only because a "future directions" chapter is exactly where
an unmerged branch quietly becomes a promise. It is not one.

## 42.3 GPU offload, past one matrix multiply

Chapter 33 covered the Metal study for a single `sgemm`, and the shape of the
next step follows from its findings rather than from ambition.

The no-copy property is real but conditional on a **blessed allocation** — a
guest `MAP_SHARED|MAP_ANONYMOUS` mapping that stays one contiguous host region
across `fork`. Anything else must decline, and the guest shim needs a CPU path
regardless. The measurement that decides go or no-go has to come from an actual
iPad, because no available quiet host can produce it: the Linux build host has
no Metal, and the available Mac has an Intel integrated GPU and a 4 KB page
size, which makes the 16 KB-page alignment failures an iDevice would hit
invisible.

And milestone 1 is asymmetric by construction: a naive kernel with a per-call
`bytesNoCopy` wrap means **a loss is not conclusive while a win is**.

Beyond that sits the standing feature request for 3D acceleration through
`virglrenderer`, which is a much larger proposition and would need the Wayland
work to land first.

## 42.4 Nested AOK, which already works

Some future directions turn out to be present tense.

**A Linux build of iSH-AOK boots and runs inside the iSH-AOK guest** — proven on
the arm64 CLI, two levels deep, exiting cleanly:

```
HELLO-FROM-THE-NESTED-GUEST
Linux ... 5.20.66-ish_aok iSH-AOK built 2026-08-31 09:32Z unoptimized aarch64 GNU/Linux
```

The cost is exactly what one would predict and worth having measured rather
than assumed: the same 20,000-iteration busybox shell loop takes under a second
on the host, one second at one level, and **49 seconds at two levels** — about
**50x per level of nesting**.

Nobody needs to run a shell fifty times slower. What the result is good for is
what it proves: that the guest is complete enough to build and run a
non-trivial, threaded, JIT-bearing C program that itself expects a working
Linux — which is a stronger statement about conformance than any test suite
makes, and it makes AOK its own most demanding workload.

## 42.5 The Bedrock companion

Bedrock-AOK is a community project — not part of this repository — that runs
multiple distributions' userlands side by side under AOK, with its own
capability dialog reporting each feature as *native* (the AOK kernel provides
it), *emulated* (its own userspace workaround) or *unavailable*.

Two things make it interesting here.

It is a **conformance report from outside the project**, produced by somebody
with different requirements — and the two capabilities it identified as gaps
were `bind_mount` and FUSE, both of which the kernel now carries. Namespaces
were explicitly *not* the ask, which matters given Chapter 41: the thing this
system architecturally cannot do turned out not to be what a multi-distribution
tool actually needed.

And the honest status: the probe itself is still unverified from this side. The
check comes when the tool's author runs the capability dialog against a release
carrying both features. If either still grades unavailable, the next step is to
read their probe rather than to assume the report is wrong.

## 42.6 The thought experiment

`docs/wasm_browser_architecture.md` works through what a WebKit-hosted iSH-AOK
would look like: a native shell app with a `WKWebView`, a terminal UI in
JavaScript, the CPU core compiled to WebAssembly, and syscalls handled by a
JavaScript shim that either maps onto browser storage and network APIs or
bridges back to native code.

It is filed as an architecture note rather than a plan, and it earns its place
in the tree for a reason worth naming: **writing down the design you are not
building is how you find out what your current design is buying you.** The
answer, in that document's case, is most of Part IV — the moment syscalls are
answered by browser storage APIs, fakefs's uid/gid/mode/device-node model has
nowhere to live.

## 42.7 What a second maintainer would need first

This project has one primary maintainer, an unusual amount of its reasoning
written down, and a substantial fraction of that reasoning in comments rather
than documents. So the honest closing question for a book like this is: what
would somebody else need in order to work on it?

**The build reality, immediately.** Xcode is the only build that ships, meson is
a convenience, and the configuration surface between them is where features go
missing (Chapter 34). Nothing else is as expensive to learn late.

**Where the truth lives.** `docs/TODO.md` is a lab notebook, not a task list —
it holds diagnosed-but-unfixed entries with measurements, closed entries with
their full investigation, and rejected designs with reasons. The release notes
are a design record (Chapter 37). And the comments are unusually load-bearing,
with the caveat of Chapter 40: they make checkable claims, and some of them are
false.

**The oracles, before writing any code.** Chapter 9's discipline is what
separates a fix from a plausible change, and the most common expensive mistake
in this project's history is calling something a defect without asking a real
Linux first.

**And the four registration points** that decide whether work is visible: the
`/AOK` manifests (Chapter 21), the three test registration points across two
files (Chapter 9), the native program registry (Chapter 22), and the Xcode
knobs mirroring meson's (Chapter 34). Every one of them fails silently.

## 42.8 The end of the argument

This book opened by claiming that emulation quality is a fidelity problem rather
than a speed problem, and that speed is the part that turned out to be
tractable.

Both halves have now been paid for. Speed was answered by threaded code, four
translators, fusion, high-level emulation, native programs and two accelerators
— and it is answered well enough that the guest architecture stops mattering the
moment a workload becomes syscall-bound. Fidelity has no equivalent answer,
because it is not one problem. It is `mkdir -p` failing for unprivileged users,
`sudo` segfaulting before `main`, a JVM dying in its own assembler, a pipe that
reports a flag nobody set, and a shell whose command substitutions come back
empty because a completion library turned an option off.

Each of those was found by somebody running real software and asking why. None
of them was found by reading the specification. And the system is trustworthy in
proportion to how many of them have been found — which is why the most valuable
artifacts in this repository are not the JIT or the shim, but over two hundred
small programs in `tests/manual/`, each one a thing somebody once believed and
was wrong about.

The book's last recommendation is therefore the same as Chapter 40's, and it is
the only one that transfers to any system at all: **check the thing itself.**

---

*Anchors:* [docs/wayland_workspace_plan.md](../../docs/wayland_workspace_plan.md),
[docs/wayland_rotation_resize_plan.md](../../docs/wayland_rotation_resize_plan.md),
[docs/external_display_plan.md](../../docs/external_display_plan.md),
[docs/metal_sgemm_milestone1.md](../../docs/metal_sgemm_milestone1.md),
[docs/wasm_browser_architecture.md](../../docs/wasm_browser_architecture.md),
[opt/AOK/tools/start-wayland.sh](../../opt/AOK/tools/start-wayland.sh),
[opt/AOK/tools/setup-wayland.sh](../../opt/AOK/tools/setup-wayland.sh),
[docs/TODO.md](../../docs/TODO.md).
