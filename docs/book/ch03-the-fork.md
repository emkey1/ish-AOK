# 3. The fork

Forks happen for two reasons. Either somebody disagrees with a project's
direction, or somebody wants to go further in a direction the project is not
travelling. iSH-AOK is the second kind, and the difference shows in how it
behaves: it merged upstream for years, it carries upstream's contributors'
names in its own commits, and its README still tells you to use `ish-app/ish` if
what you want is upstream iSH.

## 3.1 It starts with diagnostics

The first commits from this fork's author, in **December 2020**, are not
features. They are:

- "Modify `proc_entry_stat` to get and set correct uid & gid for `/proc`
  entries"
- and, a year later in December 2021, "Print the process name as well as the pid
  when informing about 'stub syscall'"

The second one is the tell. It adds no capability. It makes a message that said
*"stub syscall 352"* say *"pid 412 (systemd-udevd) stub syscall 352"* — which is
the difference between a log line you skip and a log line that tells you what to
go and read (Chapter 11).

Nearly everything this book describes about the fork's method is visible in that
one-line change: **make the failure name its own cause**. The stub-syscall
logging, the fd-555 trace, the diagnostics store, the `printk` that names the
process holding a conflicting lock, the `[no memory sync]` line added to
ptraceomatic *after* the bug was already fixed — they are all the same instinct,
applied for six more years.

## 3.2 2022: locks, leaks, and merging upstream

The fork's first substantial year was 2022 — 279 commits — and reading the
subject lines is like reading a stability log:

```
Keep track of what function used a lock_t
Fix deadlock
Fixed significant memory leak, minor cleanup/fix
hostname now settable by root, uptime reports the time when the app was started
Added Jason Conway's Re Ordering patch (upstream PR #1944)
Merge branch 'master' of https://github.com/ish-app/ish into ish-app-master
```

Two things in that list matter for what came later.

The lock instrumentation — "keep track of what function used a `lock_t`" — is
the ancestor of `ISH_LOCKSTATS` and every measurement in Chapters 16, 17 and 36.
The fork's interest in *who holds what* predates the bugs that made it
essential by four years.

And the merges. This is a fork that tracked upstream, and it pulled in an
upstream *pull request* — Jason Conway's memory-ordering patch — that still sits
in the dispatch path of every guest instruction today (Chapter 6). A fork that
takes patches from the project it forked from is not a schism.

## 3.3 2023: reference counting, in public

The late-2023 commits are worth quoting because of their tone:

```
o Very much a WIP, and very broken.
o Refactor code in preparation for implementing reference tracking in task and
  memory allocations ... Very broken, but compiles and links (for me anyway)
o Runs, but not for long on complex workloads
o Inline most of ro_lock.c, because that makes things much faster. Who knew?
  Clearly not me.
o Fixed one rare crash the app bug, failed to fix another. Releasing anyway as
  it is still much more stable than previous releases
```

That is a public repository, on the default branch, describing the work as
broken while it is broken. The reference-counting work being done there is what
Chapter 10 describes as `task->reference` and the deferred-free path — the
machinery that lets a pidfd keep a task's memory alive without gating its exit.

It took weeks, it destabilized the app while it was in progress, and it was
narrated honestly the whole way. The releasing-anyway note is the same
calculation Chapter 41 makes about known gaps: an imperfect thing that is better
than what it replaces, stated as such.

## 3.4 What "AOK" came to mean

Somewhere in this period the fork stopped being a patch set and became a
product: its own name, its own bundle identifier (`app.ish.iSH-AOK`), its own
TestFlight, its own release numbering — already in the 500s by the end of 2023 —
and eventually its own README, because upstream's had stopped being true here.

The project's own summary of the divergence, from that README:

> This fork is not just a rebrand. It carries fork-specific behavior, bundled
> roots, diagnostics work, File Provider integration, and support for four guest
> architectures. If you want upstream iSH, use `ish-app/ish`.

The inventory as of build 551 is the table of contents of Parts II through VI:
four guest architectures (Chapter 7), native programs (Part V), `/AOK`
(Chapter 21), bundled and downloadable roots (Chapter 30), FUSE (Chapter 20),
Shortcuts (Chapter 32), the optional accelerators (Chapter 33), and the
diagnostics that started it all.

## 3.5 Fork etiquette, in practice

Three things this fork does are worth naming, because forks often do none of
them.

**It credits.** The arm64 guest work is "motivated by, and in places adapted
from" `OpenMinis/ish-arm64`, a GPLv3 fork of the same upstream that added the
same capability independently — with file-level attribution in
`docs/CREDITS-aarch64.md` and a plan document that says plainly "this is not a
clean-room reimplementation" (Chapter 7).

**It keeps the identifiers.** The binary is still `ish`. The library is still
`libish.a`. The environment variables are still `ISH_*`, the log function is
still `ish_printk`, and the submodule URL is still `ish-app/linux`. The project
renamed itself and deliberately did not rename the things other people's scripts
depend on.

**And it tells you when it is not the right project.** The README's Upstream
Relationship section warns that upstream instructions may be wrong here, that
branch names differ, and — a practical detail that has cost people time — that
`gh` in a clone with an `upstream` remote will resolve to `ish-app/ish` and
answer about upstream's workflows and releases instead of this fork's.

## 3.6 What a fork inherits that is not code

The Foreword counted files: 149 of the 255 core source files were created
upstream. But the more interesting inheritance is not the code.

It is `ptraceomatic`, which means the fork inherited a *standard of evidence*
before it wrote anything. It is the fakefs design, which means every later
decision about ownership and metadata had a shape to fit into. It is the
threaded-code JIT, which means the fork never had to answer "how do we run code
at all on this platform" and could spend six years on "how do we run *more* code,
*correctly*".

And it is `LICENSE.IOS` — a promise made by other people, before this fork
existed, that derived apps could ship.

Chapter 4 is about what the fork did with that inheritance in the year it
tripled the size of the tree.

---

*Anchors:* `git log --author=emkey1 --reverse`, [README.md](../../README.md)
("What This Fork Adds", "Upstream Relationship"),
[docs/CREDITS-aarch64.md](../../docs/CREDITS-aarch64.md),
[app/iSH.xcconfig](../../app/iSH.xcconfig), [CLAUDE.md](../../CLAUDE.md)
(the naming rule and its identifier exceptions),
[ch00-foreword.md](ch00-foreword.md).
