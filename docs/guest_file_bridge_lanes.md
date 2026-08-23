# GuestFileBridge: latency lanes and cancellation

Design note, written 2026-08-23 before the implementation. It covers what the
latency classes are, what ordering each caller actually needs, how ordering is
enforced once there is more than one queue, and what happens to work whose
caller has gone away.

Read [workspace_file_manager_plan.md](workspace_file_manager_plan.md) first for
what the bridge is; this note only changes how work is scheduled on it.

---

## The problem

`app/GuestFileBridge.m` created one queue and put everything on it:

```objc
_ioQueue = dispatch_queue_create("app.ish.guestfilebridge.io", DISPATCH_QUEUE_SERIAL);
```

Directory listings, `stat`, `statfs`, `mkdir`, rename, delete, whole-file reads
and writes, chunked extraction to a temp file, and cross-backend copies all ran
on that one serial queue, so any long operation blocked every short one behind
it.

That is not a throughput problem, it is a latency-class problem. A 300 MB
extraction is *supposed* to take a minute. What is not acceptable is that a
`readdir` of twelve entries queued behind it also takes a minute. And the file
manager makes that maximally visible: `WorkspaceFileManager -setLoading:`
(app/WorkspaceFileManager.m:580) sets `_tableView.userInteractionEnabled = NO`
for the whole load, so the folder is not merely stale, it is untappable. That is
the mechanism behind the reports of the file manager going "completely
unresponsive trying to do anything with the folder, including just viewing it".

The clients sharing the queue are app/ShellFileBrowser.m,
app/WorkspaceFileManager.m, app/WorkspaceImageViewer.m,
app/WorkspaceVideoPlayer.m, app/WorkspaceMarkdownViewer.m,
app/DisplayViewController.m and app/Roots.m.

## What the fix is not

**Not "make it concurrent."** AOK's fakefs metadata mutex already saturates
under a *single* thread — measured at 78% duty with `ISH_FAKEFS_LOCKSTATS` — and
parallel metadata work measured *slower*, not faster. A concurrent
`dispatch_queue` sized to the core count would make every number worse and fix
nothing, because the listing would still be sharing a lock with the copy.

The goal is **separating latency classes**, so a bulk transfer cannot sit in
front of a listing. Two serial lanes, never a concurrent queue. Two threads in
the VFS at once is a bounded and deliberate increase in contention: the bulk
transfer gets slower because the listing now interleaves with it, which is
precisely the trade being bought.

**Not "split the queue and hope."** Callers do write-then-reload — create a
folder, rename, duplicate, delete, then `-reload` — and expect the listing to
show the result. A second queue that lets a reload overtake the write it should
follow would make the user's new file intermittently not appear. Section
"Ordering" below establishes exactly which orderings are load-bearing.

---

## Latency classes

Cost is stated as worst case, because the lane has to be chosen at enqueue time
from information available without touching the VFS.

| Entry point | Cost | Lane |
| --- | --- | --- |
| `-foregroundDirectoryForTTYType:number:` | O(1) pid lookup + one `getpath` | interactive |
| `-statAtGuestPath:` | O(path depth) | interactive |
| `-filesystemStatusAtGuestPath:` | O(path depth) + one `statfs` | interactive |
| `-listDirectoryAtGuestPath:` | O(entries) | interactive |
| `-readFileAtGuestPath:maxBytes:` | O(min(size, cap)) | by cap |
| `-writeData:toGuestPath:` | O(data length) | by length |
| `-createDirectoryAtGuestPath:` | O(path depth) + `mkdir` | interactive |
| `-moveItemAtGuestPath:toGuestPath:` | one `rename`, **or** O(size) across stores | by store |
| `-copyItemAtGuestPath:toGuestPath:` | O(size), always | bulk |
| `-removeItemAtGuestPath:recursive:` | O(1), or O(tree) when recursive | by flag |
| `-extractToTempFileAtGuestPath:` | O(path depth) preamble, then O(size) | two-phase |
| `-clearExtractionCache` | O(cached files), host filesystem only | bulk |

Three of those splits need a rule that costs no I/O:

**By store.** `-hostURLForRealfsGuestPath:` is pure string work: it answers
realfs-vs-fakefs for a path without a single VFS call. A move within one store
is one `rename`; a move across stores is a streaming copy plus a delete. So
`moveItem` picks its lane from the two paths' stores, decided before anything is
enqueued.

**By flag.** `removeItem` with `recursive:NO` is one `unlink`/`rmdir`.
`recursive:YES` is an unbounded tree walk, so it is bulk — even for an empty
folder, where it will simply finish immediately on the bulk lane.

**By cap.** A read's cost is bounded by the cap the *caller chose*, and choosing
that cap is the caller declaring how much work it is asking for. The caps in the
tree today are 8 KiB (file-manager preview), 64 KiB (`/etc/passwd`), 4 MiB
(markdown viewer) and 64 MiB (image viewer). The threshold is **8 MiB**, which
puts every viewer that is reading a document on the interactive lane and the
64 MiB image read — genuinely a transfer — on the bulk lane. A future caller
picks its lane by picking its cap, and that is documented in the header rather
than left to be discovered.

**Two-phase extraction.** `extractToTempFile` has an O(1) fast path that
mattered and was being lost: a realfs-backed path returns the host URL with no
copy at all, and an unchanged file returns a cached temp file without touching
the VFS. Those checks, plus the initial `stat`, now run on the **interactive**
lane; only the actual chunked copy hops to the bulk lane. So the image viewer's
Share of an already-extracted file answers immediately instead of queueing
behind a video, and `Roots.m`'s import of a `/AOK/persist` tarball never leaves
the interactive lane at all.

Note that `listDirectory` is O(entries) and stays interactive. It is the
workload being protected, not a bulk operation — but it *is* the one interactive
operation that can block its own lane, which is what makes cancellation
(below) part of this design rather than a nice-to-have.

---

## Ordering

### What each caller actually needs

Every call site was read. The result is worth stating plainly, because it
determines the whole design:

**No caller depends on the bridge's total order. Every ordering dependency in
the app is expressed through a completion callback.**

- `ShellFileBrowser`: New Folder, Duplicate, Rename and Delete each issue
  `[weakSelf reload]` *from inside the mutation's completion block*
  (app/ShellFileBrowser.m:430, :462, :483, :505). The completion runs on main
  after the mutation has finished on its lane, so the listing is not enqueued
  until the write is already done.
- `WorkspaceFileManager`: identical shape for the same four operations
  (app/WorkspaceFileManager.m:1058, :1088, :1124, :1172), and
  `-workspaceOpenFileAtGuestPath:` navigates from inside a `stat` completion.
- `WorkspaceFileManager -reload` does enqueue two operations back to back —
  `listDirectory` then `filesystemStatus` — but they are independent reads of
  the same path with no ordering relationship, and both are interactive.
- `WorkspaceImageViewer -loadPath:` likewise enqueues a sibling `listDirectory`
  and a `readFile`; independent.
- `Roots.m`, `DisplayViewController`, `WorkspaceMarkdownViewer` and
  `WorkspaceVideoPlayer` each issue a single operation at a time.

The `ioQueue` property was published on the header precisely so callers *could*
order their own follow-up work behind pending bridge operations. It has **no
users** — the only `ioQueue` references elsewhere in the tree belong to
`MotePadDocumentStore`, a different class with its own queue.

### The guarantee that is kept

> An operation's completion runs after that operation's effects are visible to
> any bridge operation enqueued later.

This is what write-then-reload needs, it is strictly stronger than queue order,
and splitting lanes does not weaken it.

### The guarantee that is deliberately dropped

> All bridge operations app-wide observe one total order.

Nothing uses it, and preserving it *is* the bug. The `ioQueue` property is
removed from the header along with it, so nobody can rebuild a dependency on an
order that no longer exists.

### The safeguard, so this does not rest on caller discipline

Caller discipline is true today and will be violated eventually. Rather than
document a rule and hope, the bridge enforces per-path ordering itself, under
one invariant:

> **The interactive lane never waits.** If an interactive operation would have
> to wait for anything, it is *moved to the bulk lane* instead. The bulk lane
> may wait, because a bulk operation is already slow by construction.

Concretely, the bridge keeps a small table of the *claims* held by work queued
or running on each lane (operations that name two paths, move and copy, claim
both). Two claims conflict when they reach any of the same filesystem state:

- **the same path**, or
- **a path and its parent directory** — a write at `D/name` changes `D`'s entry
  list, and moves `D`'s mtime, so a listing or stat of `D` is ordered against it;
- **a subtree and anything inside it** — but only for an operation that actually
  reaches that far, which is the recursive delete alone. Its claim carries a
  `subtree` flag; nothing else does.

The first draft of this rule was "equal, or one is an ancestor of the other",
and it was wrong in the direction that matters. `/` is an ancestor of
everything, so a copy into `/tmp/foo` conflicted with a listing of `/` — and any
transfer anywhere would have pushed every listing of `/` onto the bulk lane,
which is the head-of-line blocking this design exists to remove. A write at `P`
does not change what `/` contains unless `P` is a direct child of it.

- **Bulk in front of interactive.** An operation bound for the interactive lane
  whose path overlaps a pending bulk path is enqueued on the **bulk lane**
  instead, landing behind the operation it must follow. It is slow — it has to
  be, it is ordered behind bulk work on its own path — but the interactive lane
  is not blocked, and listings of every *other* directory stay fast. That is the
  entire point.
- **Interactive in front of bulk.** An operation running on the bulk lane whose
  path overlaps a pending interactive path first drains the interactive lane
  with a `dispatch_sync` barrier. Bounded by interactive-operation cost, and
  only on an actual overlap.

This is deadlock-free by construction: the interactive lane never waits on the
bulk lane — it is *routed*, which is an enqueue, not a wait — so the wait graph
has no cycle. The routing decision is made under a lock that is released before
any waiting.

What this buys: **per-path ordering is preserved in both directions**, whether
or not the caller chains through completions. What it does not buy, and does not
try to: a global order across unrelated paths. A listing of `/etc` and a copy
into `/home` have no ordering relationship and never did.

### One visible consequence, accepted

A cross-store streaming copy writes to `.NAME.guestfilebridge-tmp` beside its
destination and renames on completion. Previously no listing could run during a
copy, so that temp file was never observable. Now a listing of the destination
folder during a copy can see it. It is dotted, so it is hidden unless "show
hidden files" is on, and it is exactly what a guest shell watching the same
directory would see. Accepted rather than papered over.

---

## Cancellation, and what happens when a caller goes away

Listings had no cancellation at all: `-listDirectoryAtGuestPath:` returned void,
took no token, and neither its `readdir` loop nor its per-entry `stat` loop had
any cancellation check. Navigating away or dismissing the sheet left the whole
enumeration running to completion — `ShellFileBrowser`'s `_loadGeneration` guard
discards the *answer*, not the *work*. On a big directory that is seconds of
abandoned VFS work still holding the lane.

The bridge already had the right shape to copy in `ISHGuestFileExtractionToken`
plus `-cancelExtraction:`. That is generalised:

- `ISHGuestFileOperationToken` is the token type;
  `ISHGuestFileExtractionToken` stays as an alias so existing call sites compile
  unchanged.
- `-cancelOperation:` cancels any token; `-cancelExtraction:` remains as a
  synonym.
- `-listDirectoryAtGuestPath:completion:` now *returns* a token. Changing the
  return type from `void` is source-compatible with every existing call site,
  since they all ignore it.
- Cancellation is polled in the `readdir` loop, the per-entry `stat` loop, the
  host-directory listing loop, the whole-file read loop, the extraction chunk
  loop (which already had it), and the streaming-copy chunk loop (which did
  not — a cancelled copy used to run to completion).
- A token cancelled before its block runs is dropped at the head of the block,
  so an operation that never started costs nothing.

Callers wire it to their existing lifecycle: both browsers cancel the previous
listing when a new one starts and when they are torn down, and the image viewer
cancels its sibling scan the same way.

**Recursive delete is deliberately not cancellable.** Stopping a tree walk
halfway leaves a half-deleted tree and no way to tell the user what survived. A
delete that has begun runs to completion; that is a correctness choice, not an
oversight.

---

## Shared state under two lanes

Three pieces of bridge state stopped being single-threaded and are now guarded:

- `_extractionCache` was documented "accessed only on ioQueue". The lookup now
  runs on the interactive lane and the store after the bulk copy, so it moves
  under the same `@synchronized(self)` monitor the token bookkeeping already
  used.
- `_inflightExtractions` / `_cancelledExtractions` were already `@synchronized`.
- The lane routing table is new, and guarded by the same monitor. The lock is
  never held across a `dispatch_sync`.

`current` — the borrowed task pointer that `fs/path.c` resolves through — is
`__thread` (kernel/task.h:319), so two lanes borrowing pid 1 on two GCD threads
is safe. `-withGuestTaskContext:` sets and restores it within a single block, so
GCD moving a serial queue between threads across blocks is also fine. Two bridge
threads in the VFS is no different from two emulated guest threads, which the
VFS handles all the time.

---

## Verification

Building it is not evidence. The claims that need testing are:

1. **A listing completes promptly during a bulk transfer.** Start a large copy
   or extraction, then list a directory and time it.
2. **Write-then-reload still shows the result** — including the racy shapes
   (duplicate then reload, delete then reload, new folder then reload) run
   repeatedly rather than once, since an intermittent reorder is exactly the
   failure a naive split would introduce.
3. **An abandoned listing actually stops**, rather than running to completion
   with its answer discarded.

Two knobs make that measurable rather than a matter of opinion:

- `ISH_BRIDGE_LANE_SELFTEST=<MiB>` runs a harness at launch, against the live
  guest filesystem through the public API, and logs a PASS/FAIL summary.
- `ISH_BRIDGE_SINGLE_LANE=1` collapses the two lanes back into one queue — the
  behaviour this design replaced — so the control is taken on the same hardware,
  in the same binary, in the same run.
- `ISH_BRIDGE_LANE_LOG=1` logs one line per operation with its lane, whether it
  was re-routed, how long it waited to start and how long it ran.

Results, and the bugs the testing found in the first draft of this design, are
recorded in [TODO.md](TODO.md).
