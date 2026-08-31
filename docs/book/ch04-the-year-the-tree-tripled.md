# 4. 2026: the year the tree tripled

| year | commits |
|---|--:|
| 2017 | 336 |
| 2018 | 905 |
| 2019 | 604 |
| 2020 | 469 |
| 2021 | 609 |
| 2022 | 759 |
| 2023 | 221 |
| **2026** | **2,216** |

Six years of upstream and fork together produced about 3,900 commits. 2026
produced 2,216 — more than half the project's history, in one year, on a tree
that was already mature.

This chapter is about what that year contained and how it was worked, because
the *method* is what made the volume possible and it is the part that
generalizes.

## 4.1 The shape of the year

| month | commits | what landed |
|---|--:|---|
| February | 28 | restart |
| March | 107 | groundwork |
| April | 400 | **amd64** — the ABI split |
| May | 59 | consolidation |
| June | 373 | conformance, accelerators |
| July | 660 | **arm64** (1st), **riscv64** (10th) |
| August | 589 | **native programs**, and the conformance sweeps |

Three guest architectures in four months, then a mechanism that runs host code
as guest programs, then a month of finding out what all of it had broken.

The dip in May is not idleness. It follows the largest single structural change
in the project's history — the amd64 port, which is mostly the removal of the
assumption that "guest" means 32 bits (Chapter 7) — and the pattern of a big
structural month followed by a quiet one recurs.

## 4.2 Why three architectures took four months

The plan documents answer this, and the answer is compounding.

**amd64** (April) paid for the infrastructure. Its plan opens with a list headed
*Hard Blockers*: guest-sized types are 32-bit and leak through memory
management, exec, syscall marshalling, signals and ptrace; CPU state models
eight registers and `eip`; the decoder treats `0x40`–`0x4f` as INC/DEC; memory
management is a fixed 4 GB design; the ELF loader rejects anything but
`ELFCLASS32`. Removing nine years of assumptions is most of what that month was.

**arm64** (1 July) was narrower because that work existed — and because
`OpenMinis/ish-arm64` had already demonstrated the capability, so the plan could
adapt their public source with attribution rather than start from an idea.

**riscv64** (10 July, nine days later) was the third 64-bit asm-generic guest
and the second gadget engine, so nearly every kernel-layer decision was already
made. The plan's porting rule is the whole story: *grep `GUEST_ABI_ARM64` and
mirror every site.*

The compounding is the point. The first port is a rewrite; the second is an
adaptation; the third is a checklist.

## 4.3 What made the pace possible

The year was worked with AI assistance, and the honest account of that is
neither "it wrote the code" nor "it made no difference". What it did was change
which activities were expensive.

Reading is cheap. A conformance sweep that reads every `*at()` syscall against
Linux's documented behaviour, or every `/proc` file against what a real
`/proc` produces, is a large amount of careful comparison — and Chapters 15, 16
and 18 are full of five-defects-in-one-commit entries that came from exactly
that.

Which shifts where the risk is. When generating a plausible change is cheap, the
scarce thing becomes **evidence that the change is right** — and that is why
every mechanism in this book's Part VII exists at the scale it does:

- Every change validated by a guest-side test, a differential oracle
  (Chapter 9), or a measurement with its method recorded (Chapter 38).
- A gate whose allowlist is deliberately too small (Chapter 23).
- A/B before re-runs, and a widened race window with counts before and after
  (Chapters 14 and 35).
- And postmortems written into `docs/TODO.md` *while the answer was still known*,
  which is why this book could be written from primary sources at all.

The failure mode this guards against is specific and worth naming: a change that
is plausible, compiles, passes the tests that exist, and is wrong. Chapter 40's
nine rules are all, read one way, defences against exactly that — and several of
them (*check the oracle before claiming a defect*, *re-derive a recorded
diagnosis*, *prove the instrument before believing a negative*) were written down
in 2026 because they were violated in 2026.

## 4.4 The record it left

The year's most valuable artifact may not be the code.

`docs/TODO.md` is 2,163 lines and it is not a task list. It is a lab notebook:
diagnosed-but-unfixed entries with measurements and rejected designs; closed
entries carrying their full investigation, including the wrong first hypothesis;
and a "Deferred on purpose" section holding work that was built and judged
inadequate (Chapter 41).

The release notes for builds 521 through 551 are a second record, written for
users and detailed enough to reconstruct the engineering (Chapter 37).

And the code comments are a third, dense enough that this book has quoted them
on nearly every page — with the caveat of Chapter 40, that they make checkable
claims and some of them are false.

Twenty-seven releases in one year, roughly one every twelve days, each with
notes.

## 4.5 What the year did not change

It is worth ending Part I by noting what six years of upstream and one
extraordinary year of forking did *not* alter.

The engine is still threaded code, and `gen()` is still the function written on
26 May 2018. The filesystem is still bytes plus SQLite. A process is still a
thread with `current` in thread-local storage. Correctness is still established
by differential comparison, using a harness from week three of the project's
life.

Everything in Parts II through VI is either that foundation, or something added
to it. 2026 tripled the tree without replacing what the tree was, which is the
strongest thing that can be said about the design decisions of 2017 — and the
reason the Foreword is where it is.

Part II starts at the bottom, with the guest machine.

---

*Anchors:* `git log --since=2026-01-01`,
[docs/TODO.md](../../docs/TODO.md),
[docs/release-notes-since-iSH-AOK_521.md](../../docs/release-notes-since-iSH-AOK_521.md)
through `_551.md`, [docs/amd64_port_plan.md](../../docs/amd64_port_plan.md),
[docs/aarch64_guest_plan.md](../../docs/aarch64_guest_plan.md),
[docs/riscv64_guest_plan.md](../../docs/riscv64_guest_plan.md).
