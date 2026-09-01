# 35. Testing strategy

Chapter 9 was about *truth*: where an emulator gets an authoritative answer to
"what should this do", and how to compare against it. This chapter is about
*process* — which tests exist, what each one can uniquely see, what runs
automatically, and the specific ways a testing system can be green and wrong.

## 35.1 Four tiers

| tier | what it is | what only it can see |
|---|---|---|
| host unit tests | `meson test -C build` | pure logic — `float80`, decoders, utilities |
| guest suite | ~190 C programs run **inside** a guest at `/AOK/tests` | kernel behaviour as a real userland experiences it |
| end-to-end | boot an i686 Alpine, compile C in it, run the result | fork- and exec-heavy work on an architecture daily testing never touches |
| differential | ptraceomatic, unicornomatic, the conductor | instruction- and program-level divergence from real Linux |

The important column is the third one. These tiers are not levels of thoroughness
— they are different *vantage points*, and a bug visible from one may be
invisible from every other.

## 35.2 The incident that justifies keeping all of them

> **The bug that taught us this**
>
> A change passed the guest regression suite **125 passed, 0 failed** on arm64.
>
> It broke the end-to-end suite **three runs out of three**.
>
> It was the lazy-mmap fork bug of Chapter 5. The arm64 guest suite could not
> see it; CI could, because the e2e suite uses an i686 root, compiles C inside
> the guest, and therefore exercises fork- and exec-heavy work on a second
> architecture with a different libc.

Suites stop being redundant the moment their architecture, their libc, or their
workload differs. The overlap between them is the part you could afford to lose;
the difference is the part that catches things.

Which produces a pre-push rule the project states explicitly, and it is worth
having in one place:

> The guest regression suite is **not** enough before a push. Both suites, plus
> the JVM (`java -version` in the alpine root) for anything touching memory or
> exec.

That is the *pre-push* gate: both suites and the JVM, on one guest. The
*release* gate is wider — all four guests, on the Mac and again on real
hardware, because a 32-bit ABI difference is invisible on the other three and a
device regression is invisible everywhere else. Section 37.4 has it.

The JVM is in that list for a specific reason: it is the most demanding ordinary
program available. It threads heavily, maps aggressively, and uses `exec` and
signals in combinations nothing else does — so it functions as a
whole-system smoke test that no hand-written case would cover.

## 35.3 What runs automatically

Eight workflows, and the interesting thing is which of them gate anything:

- **`ci.yml`** — a Linux build on `ubuntu-24.04`, with a matrix of **clang and
  gcc**. This is the one that catches things.
- **`build-dev-ipa.yml`** — the app, on `macos-15`, on push.
- **`build-release-ipa.yml`** — triggered by a `builds/iSH-AOK_*` tag
  (Chapter 37).
- **`build-ktop.yml`** — cross-compiles `ktop`'s prebuilt binaries and commits
  them back. Section 35.5 is about what that means.
- **`deploy-site.yml`**, **`update-alpine-repo.yml`** (scheduled),
  **`autolabel.yml`** — supporting infrastructure.
- **`upload-build.yml`** — inherited from upstream, gated on
  `github.repository == 'ish-app/ish'` and pinned to a retired `macos-12`
  runner, so it never runs here at all.

Note what is *not* in that list: the guest regression suite and the differential
harnesses. The end-to-end suite *is* in CI — `ci.yml`'s Linux job finishes with
`meson test -C build e2e`, which is how Section 35.2's fork bug was caught. The
other two need a booted guest and, in practice, a device or a Mac with a real
toolchain — so they are run by a person, deliberately, before a push and before
a release.

That is an honest limitation rather than a boast. A gate somebody has to
remember is a gate that will occasionally be skipped, and Section 35.5 is about
what happens when nobody notices something has stopped running.

## 35.4 Linux CI is a second compiler

The Linux build does not ship. Nobody runs iSH-AOK on Linux, `build-mac` and the
dev IPA are what users get, and the Linux job could be deleted tomorrow without
affecting a single user.

It is kept green anyway, for a reason worth stating precisely:

> **GCC is a second opinion on the code that DOES ship.**

> **The bug that taught us this**
>
> Fixing the Linux build turned up a live iOS crash.
>
> `bash --rcfile FILE` and `--init-file FILE` wrote through a NULL pointer, and
> because native bash runs in-process (Chapter 22), the app went down.
>
> Clang accepted the bad assignment **silently** — the native program builds all
> pass `-w` — and GCC rejected it as `-Wincompatible-pointer-types`.
>
> Nothing about Linux was involved. Only the compiler.

Two things compound there. Vendored code is compiled with warnings off, because
somebody else's project produces thousands of them and nobody will read them —
so the compiler that ships is silent *by configuration*. And a second compiler,
on a platform nobody uses, is the only thing looking at that code with warnings
on.

The rule: **treat a GCC-only diagnostic as a possible shipping bug, not as "GCC
being fussy".**

Keeping it green has a dominant cost, and the fix pattern for it is
counter-intuitive enough to record:

> The dominant breakage class is generated `config.h` files: bash's, zsh's and
> OpenSSH's are each produced by running `configure` **on a Mac**, and the same
> tree compiles for Linux, so they assert Darwin facts that are false on glibc —
> `strtonum`, `memset_s`, `timingsafe_bcmp`, `<util.h>`,
> `pw_class`/`pw_change`/`pw_expire`, `SOCK_HAS_LEN`, `HAVE_DECL_FPURGE`,
> `XATTR_EXTRA_ARGS`.
>
> Fix by guarding each with `!__linux__` so the shipping build is bit-for-bit
> unmoved — **never by regenerating, which would move Darwin.**

That is the key insight and it generalizes. When a committed, generated artifact
is correct for the platform that ships and wrong for a platform that only
tests, the fix goes in the *consumer*, not the artifact. Regenerating to please
the test platform changes the thing you are shipping in order to fix something
nobody runs.

And one small portability fact that costs a build every time it recurs:
**`__thread` must follow the storage class for GCC** — `extern __thread`, not
`__thread extern`. Clang takes either order.

## 35.5 A failing job can freeze an artifact

This is the most instructive CI failure in the project, and it is a category
rather than an incident.

> **The bug that taught us this**
>
> `build-ktop.yml` cross-compiles `ktop`'s prebuilt binaries **and commits them
> back to the repository**.
>
> It had failed on **every run since before build 547** — four releases.
>
> Nothing broke. Everything built. Everything shipped. The prebuilt binary
> simply stopped tracking `ktop.c`, so the shipped `ktop` still had the exact
> architecture-column bug a user had reported and that had been *fixed in
> source*.
>
> The failure was never the project's code. The job pinned a setup action that
> resolves a requested toolchain version against a dev-builds path plus
> community mirrors — and the version requested was a *release*, served from a
> different path entirely. Every mirror answered 404 or 503, and the job died at
> "Install zig" before compiling anything.

Two lessons, and the second is the one that matters.

A **red build that blocks nothing will stay red**, because nothing forces
anybody to look. `ci.yml` failing is noticed because it is on every pull
request; `build-ktop.yml` failing is a badge nobody has reason to check.

And **a fixed bug that is still shipping is worse than an unfixed one**, because
the report gets closed. The user who reported the architecture column had been
told it was fixed, and it was — in a file nobody was building.

Any job that both produces an artifact and commits it back needs its freshness
checked *from the artifact side*: a build that compares the committed binary
against a fresh compile, or a version stamp that a release check reads.

## 35.6 Telling a flake from a race

"Passes alone, fails in the suite" is the signature of a load flake. It is also
the signature of a real race that needs a warm machine, and Chapter 12's
`PTRACE_SEIZE` hang was the second kind while looking exactly like the first.

The procedure that separates them, in order, and *before* any further re-runs:

1. **A/B the suspected cause in one binary.** Flip the single thing the change
   touched; flip it back. Causal in both directions, same build. One A/B costs a
   fraction of a re-run and settles what repetition cannot.
2. **Bisect the context.** `setup-regressions.sh --only a,b,c` — if it
   reproduces with four preceding tests and not three, that rules out "the
   neighbouring test leaves state" and points at cumulative work.
3. **Look, do not theorize.** `lldb -p <pid>` and `thread backtrace all` on the
   hung process named both sides of that bug in one shot.

Two operational notes that have each cost a day:

**Never run tier0 and `xcodebuild` at the same time.** Timing-sensitive tests
time out under the load of a full Xcode build and are reported as `[HANG]`,
which reads exactly like a regression. Run the two gates in sequence.

**One documented flake exists** — `time_conformance` fails only in a full-suite
run — and its existence is precisely what makes the discipline above necessary.
Having a known flake means "it's probably the flake" is always available, and it
is wrong often enough to be dangerous.

## 35.7 What testing an emulator is for

Ordinary software testing asks whether the code does what its author intended.
That question is nearly useless here, because the author's intent was "behave
like Linux", and the interesting failures are all cases where the author
believed they had.

So the tiers divide up a different job:

- The **guest suite** encodes behaviours somebody has already been wrong about.
  It is a specification by accretion, and it grows one bug at a time.
- The **oracles** (Chapter 9) supply truth the project does not own.
- **CI** supplies a second compiler and a second platform, neither of which
  ships, both of which see things the shipping configuration cannot.
- And the **e2e suite** supplies a workload nobody designed, which is the only
  tier that can catch a bug in the interaction of things that were each tested
  separately.

None of them covers the largest category, which is behaviour nobody has thought
to ask about yet. That gap is why Chapter 16's most productive testers were
`stress-ng` passing garbage to syscalls on purpose, and a `pacman` transaction
doing something no hand-written test would have tried.

The honest summary of the strategy is: encode every mistake as a test, borrow
truth wherever it can be borrowed, keep a compiler nobody ships pointed at the
code, and accept that the next real bug will arrive from a direction none of it
covers.

---

*Anchors:* [.github/workflows/ci.yml](../../.github/workflows/ci.yml),
[.github/workflows/build-dev-ipa.yml](../../.github/workflows/build-dev-ipa.yml),
[.github/workflows/build-ktop.yml](../../.github/workflows/build-ktop.yml),
[.github/workflows/build-release-ipa.yml](../../.github/workflows/build-release-ipa.yml),
`tests/manual/setup-regressions.sh`, `tests/e2e/e2e.bash`,
`tests/remote/conductor.py`, [fs/aok-tests.manifest](../../fs/aok-tests.manifest),
[docs/TODO.md](../../docs/TODO.md) ("Build and test infrastructure").

*Story:* `ktop`'s prebuilt binary shipping a bug that had been fixed in source
four releases earlier — because the job that rebuilds it also commits it, so its
failure stopped nothing and told nobody.
