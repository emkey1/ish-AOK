# Appendix H. Further reading

## The project and its neighbours

**[`ish-app/ish`](https://github.com/ish-app/ish)** — upstream iSH. The project
this one forked from, and the place to go for iSH itself: the app, the issue
tracker, the community. The Foreword covers what it built.

**[`OpenMinis/ish-arm64`](https://github.com/OpenMinis/ish-arm64)** — a GPLv3
fork of the same upstream that added an AArch64 guest independently, and the
reference the arm64 port in Chapter 7 adapts with attribution
(`docs/CREDITS-aarch64.md`).

**Bedrock-AOK** — a community project running several distributions' userlands
side by side under AOK, with a capability dialog that grades each feature
native, emulated or unavailable. Chapter 42.

## Inside this repository

The book was written almost entirely from primary sources, and these are the
ones worth reading directly:

**`docs/TODO.md`** — 2,163 lines of lab notebook. Diagnosed-but-unfixed entries
with measurements and rejected designs; closed entries with their full
investigation, including the wrong first hypothesis; and a "Deferred on purpose"
section. The single most informative file in the tree.

**`docs/release-notes-since-iSH-AOK_521.md` … `_551.md`** — written for users and
detailed enough to reconstruct the engineering (Chapter 37).

**The port plans** — `docs/amd64_port_plan.md`, `docs/aarch64_guest_plan.md`,
`docs/riscv64_guest_plan.md`. Read in that order they show a first port being a
rewrite, a second an adaptation, and a third a checklist.

**`docs/perf_benchmarks_2026_08.md`** — the derivation of the 6.8 ns dispatch
constant, with method (Chapter 38).

**`docs/guest_file_bridge_lanes.md`** — the best short piece of
performance-engineering writing in the tree, and a model for ruling out the
wrong fixes before proposing one (Chapter 31).

**`opt/AOK/docs/`** — the 21 documents compiled into the app. User-facing, and
notably honest: `llm-chat.md` warns its own readers about prompt injection in
the paragraph explaining how to disable confirmation.

**`kernel/task.h`** — not a document, but read it top to bottom anyway. Most of
its fields carry the bug that required them.

## Background, for the techniques

**Threaded code** — the representation behind Chapter 6 comes from Forth
implementations of the 1970s. Any treatment of direct, indirect, token and
subroutine threading will explain the family; the property that matters here is
that direct threading's "program" is data.

**The GPL and the App Store** — the FSF's writeups on the GNU Go (2010) and VLC
(2011) removals set out the "further restrictions" argument of GPL section 6
that Chapter 26 turns on.

**Apple TN2277, *Networking and Multitasking*** — the documented behaviour behind
`fs/sockrestart.c`: what happens to a listening socket when an app is suspended
(Chapter 19).

**RFC 8439** — ChaCha20 and Poly1305, the specification the crypto accelerator
implements and self-tests against at startup (Chapter 33).

**The FUSE protocol** (`linux/fuse.h`, version 7.31) — the wire format
Chapter 20 implements, and the reason a raw-protocol test can run on a real
Linux kernel as an oracle.
