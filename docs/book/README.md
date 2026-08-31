# The book

Full chapter drafts for the outline in [../book-outline.md](../book-outline.md).
One file per chapter, drafted in order. Anchors in each chapter link into the
tree; every measurement quoted was re-run on `working` at drafting time.

| ch | title | status |
|---|---|---|
| — | [Foreword: on standing somewhere](foreword.md) | draft |
| 1 | [The impossible app](ch01-the-impossible-app.md) | draft |
| 2 | Upstream iSH, 2017–2023 | — |
| 3 | The fork | — |
| 4 | 2026: the year the tree tripled | — |
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

Chapters 20–42 follow the outline. Parts II (5–9) and III (10–15) are complete; Part IV is in progress. Drafting order is Part II first (the engine), per the outline's own production note. Conventions: `iSH-AOK`/`AOK` for this
project and `upstream iSH` for `ish-app/ish`; guest means the emulated Linux
side, host means iOS/macOS; every chapter ends with an `Anchors:` line, and
chapters that exist because of a specific failure end with a `Story:` line.
