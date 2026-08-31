# The book

Full chapter drafts for the outline in [../book-outline.md](../book-outline.md).
One file per chapter, drafted in order. Anchors in each chapter link into the
tree; every measurement quoted was re-run on `working` at drafting time.

| ch | title | status |
|---|---|---|
| 0 | [Foreword: on standing somewhere](ch00-foreword.md) | draft |
| 1 | [The impossible app](ch01-the-impossible-app.md) | draft |
| 2 | [Upstream iSH, 2017–2023](ch02-upstream-ish.md) | draft |
| 3 | [The fork](ch03-the-fork.md) | draft |
| 4 | [2026: the year the tree tripled](ch04-the-year-the-tree-tripled.md) | draft |
| 5 | [The guest machine](ch05-the-guest-machine.md) | draft |
| 6 | [Threaded code: the JIT that is not a JIT](ch06-threaded-code.md) | draft |
| 7 | [Four guests](ch07-four-guests.md) | draft |
| 8 | [High-level emulation](ch08-high-level-emulation.md) | draft |
| 9 | [Proving an emulator right](ch09-proving-it-right.md) | draft |
| 10 | [Processes, threads, and the task table](ch10-processes-and-threads.md) | draft |
| 11 | [Syscalls](ch11-syscalls.md) | draft |
| 12 | [Signals, job control, and ptrace](ch12-signals-job-control-ptrace.md) | draft |
| 13 | [Memory management from the kernel side](ch13-memory-from-the-kernel.md) | draft |
| 14 | [Waiting](ch14-waiting.md) | draft |
| 15 | [Loading a program](ch15-loading-a-program.md) | draft |
| 16 | [The VFS](ch16-the-vfs.md) | draft |
| 17 | [fakefs: a filesystem in a database](ch17-fakefs.md) | draft |
| 18 | [The synthetic filesystems](ch18-synthetic-filesystems.md) | draft |
| 19 | [Sockets and networking](ch19-sockets.md) | draft |
| 20 | [FUSE](ch20-fuse.md) | draft |
| 21 | [/AOK: a filesystem compiled into the binary](ch21-the-aok-filesystem.md) | draft |
| 22 | [A native program is a function call](ch22-a-native-program-is-a-function-call.md) | draft |
| 23 | [The shim: answering questions about the guest](ch23-the-shim.md) | draft |
| 24 | [The thing that cannot fork](ch24-the-thing-that-cannot-fork.md) | draft |
| 25 | [The catalogue](ch25-the-catalogue.md) | draft |
| 26 | [Licensing, honestly](ch26-licensing-honestly.md) | draft |
| 27 | [What native programs cost](ch27-what-native-programs-cost.md) | draft |
| 28 | [The iOS app around the kernel](ch28-the-app-around-the-kernel.md) | draft |
| 29 | [The terminal](ch29-the-terminal.md) | draft |
| 30 | [Roots](ch30-roots.md) | draft |
| 31 | [Files, Workspace, and the app-side tools](ch31-files-workspace-tools.md) | draft |
| 32 | [Devices and system integration](ch32-devices-and-integrations.md) | draft |
| 33 | [The optional accelerators](ch33-accelerators.md) | draft |
| 34 | [Two builds](ch34-two-builds.md) | draft |
| 35 | [Testing strategy](ch35-testing-strategy.md) | draft |
| 36 | [Debugging a system with no debugger](ch36-debugging.md) | draft |
| 37 | [Releasing](ch37-releasing.md) | draft |
| 38 | [Where the time actually goes](ch38-where-the-time-goes.md) | draft |
| 39 | [Six optimizations, in full](ch39-six-optimizations.md) | draft |
| 40 | [What this project believes](ch40-what-this-project-believes.md) | draft |
| 41 | [The honest gaps](ch41-the-honest-gaps.md) | draft |
| 42 | [Where it could go](ch42-where-it-could-go.md) | draft |

All 42 chapters are drafted, and the eight appendices are in [appendices/](appendices/) — four of them generated from the tree by `appendices/generate.py`. Parts II (5–9), III (10–15), IV (16–21), V (22–27) VI (28–33), VII (34–37), VIII (38–39) and IX (40–42) are complete, as is Part I (1–4). Drafting order is Part II first (the engine), per the outline's own production note. Conventions: `iSH-AOK`/`AOK` for this
project and `upstream iSH` for `ish-app/ish`; guest means the emulated Linux
side, host means iOS/macOS; every chapter ends with an `Anchors:` line, and
chapters that exist because of a specific failure end with a `Story:` line.
