# 37. Releasing

A release is where every constraint in this book arrives at once. The build has
to be the right one (Chapter 34). The licence decision has to have been made
(Chapter 26). The documentation compiled into the app has to be true
(Chapter 21). The tests that are not in CI have to have been run by somebody who
remembered (Chapter 35). And the artifact has to work on devices nobody has.

This chapter is about the machinery, and about the parts that are not machinery
and therefore fail differently.

## 37.1 The tag is the build system

The release process is short:

1. Bump `CURRENT_PROJECT_VERSION`.
2. Add `docs/release-notes-since-iSH-AOK_<N>.md` and a summary.
3. Tag that commit `builds/iSH-AOK_<N>` and push it.

Pushing that tag triggers `.github/workflows/build-release-ipa.yml`, which
builds the IPA, publishes a GitHub release, and prunes to the five most recent —
the oldest numbered release and its IPA asset are deleted, though the git tag
survives.

**The tag name is load-bearing.** The workflow triggers on `builds/iSH-AOK_*`,
so a differently named tag produces no release build at all — silently, because
nothing was asked to happen and nothing did.

And there is a subtler rule about *which commit* to tag:

> **Tag the version-bump commit or later, never the notes commit.**

Two releases were tagged on their "docs: add release notes" commit, which
happened to be fine because the bump came first. Then the order inverted in a
later cycle — and tagging the notes commit there would have published an IPA
stamped with the *previous* build number, which App Store Connect rejects as
non-increasing. This repository has hit that once already.

The check is one line, and it is the kind of thing that belongs in a release
script rather than in somebody's memory:

```sh
git show <sha>:iSH-AOK.xcodeproj/project.pbxproj \
  | grep -o 'CURRENT_PROJECT_VERSION = [0-9]*' | sort -u
```

The notes filename has its own trap: it is named for the build being **shipped**,
and the workflow once looked for `<N-1>`, so five consecutive releases published
the wrong body text. A filename convention that is off by one in the producer
and the consumer produces a result that looks deliberate.

## 37.2 The IPA that had nothing to read

The best release-engineering bug in this project is about what a *sideloader*
reads, and it is a good illustration of how far a release can travel from the
code that caused it.

> **The bug that taught us this**
>
> The GitHub release IPAs are built with `CODE_SIGNING_ALLOWED=NO`. So the
> bundle has **no `LC_CODE_SIGNATURE`, no `archived-expanded-entitlements.xcent`
> and no `embedded.mobileprovision`** — verified by unzipping a published IPA.
>
> AltStore, SideStore and Sideloadly decide what to request from Apple by
> *reading the entitlements of the app they are re-signing*. They found none,
> took their "App ID has no app groups, skipping assignment" path, and installed
> iSH-AOK **with no App Group at all**.
>
> `containerURLForSecurityApplicationGroupIdentifier:` then returned nil, and the
> first-launch root import died with "No filesystem storage available". The app
> was unusable — not merely that one import.
>
> And the diagnosis was actively misdirected, because the app's own graceful
> alert reads like an entitlement misconfiguration *in this project*, which
> sends you to look at the entitlements files, which are correct.

The lesson is not about entitlements. It is that **an unsigned artifact is not a
signed artifact minus the signature** — it is missing metadata that downstream
tools use to decide what your app is allowed to do. Anything that re-signs is a
second build system you do not control and cannot see, and its inputs are
whatever your build left in the bundle.

## 37.3 `release-aok.sh`

`tools/release-aok.sh` wraps the local half:

```bash
./tools/release-aok.sh preflight                        # clean tree, export options, Xcode
./tools/release-aok.sh archive
./tools/release-aok.sh export latest /tmp/iSH-AOK-export
./tools/release-aok.sh upload-fastlane                   # TestFlight
```

`preflight` is the interesting subcommand, because its whole job is to fail
early on the conditions that otherwise fail late: an unclean tree, missing
export options, the wrong Xcode.

`upload-fastlane` drives the existing fastlane lane and needs a Ruby, Bundler
and fastlane setup plus signing and authentication secrets — which is the part
of this pipeline most likely to be broken on any given day, for reasons entirely
outside the project.

## 37.4 Two checklists that are not code

Everything above is automatable and mostly automated. Two things are not, and
both were added to the process after being missed.

**The documentation is compiled into the app.** `opt/AOK/docs/*.md` →
`fs/aok-docs.manifest` → `tools/gen-aokfs.py` → served by `fs/aok.c`
(Chapter 21). So a wrong instruction in `/AOK/docs` is worse than a stale wiki
page: it **ships to every device** and cannot be corrected until the next build.
Drift there is invisible to the project and highly visible to the user.

The audit is therefore part of every release:

- `README.md`, plus `README_KO.md` and `README_ZH.md`. Translations drift
  silently, and the practical technique is to compare the
  **language-independent** parts — headings, code blocks, commands, paths,
  tables — rather than trying to read the prose.
- Every file in `opt/AOK/docs/`: does each command, flag, environment variable
  and path still exist?
- And **coverage, not just correctness**: a feature that shipped with no
  documentation is a documentation bug too.

**Upstream reports get triaged before the tag goes out.** Open pull requests
(decide merge-or-close rather than letting them ride through another release),
open issues (catch anything this cycle already fixed — close it, and say so in
the notes — and anything that should block the tag), and **Apple's crash reports
in Xcode's Organizer**, which is where a regression that only appears on real
devices surfaces and nowhere else in this repository.

That last one is the only view the project has of failures on hardware it does
not own, which makes it the highest-value item on a list nobody enjoys.

## 37.5 The release notes are a design record

`docs/release-notes-since-iSH-AOK_521.md` through `_551.md` are in the tree, and
they are worth reading as a genre.

They are not marketing copy and they are not changelogs. A typical entry names
the symptom a user would have seen, the mechanism underneath it, and often the
reason it was not caught — which is why this book has been able to quote them so
heavily. Chapter 27's `strchrnul` crash, Chapter 18's `/proc` audit, Chapter 12's
ptrace race: all of them are in release notes, written for users, in enough
detail to reconstruct the engineering.

There is a discipline hiding in that. Notes written that way cannot be produced
at the end from a `git log` — they require having recorded, at the time, what
the symptom was and why it happened. Which means the release-notes format is
quietly enforcing the same habit `docs/TODO.md` enforces: write the postmortem
while you still know the answer.

One entry deserves quoting for its honesty about its own scope:

> The notes were written at 142 commits and the release is now 157. Fifteen of
> those are the ones a reader would most want to know about, so they were not
> something to leave out.

## 37.6 What a release actually risks

The failure modes in this chapter have almost nothing in common with the ones in
the rest of the book, and that is the point of having it as a chapter.

An emulator bug is found by a test, a user, or an oracle. A *release* bug is
found by a stranger, on a device you do not have, with a tool you did not write,
about a decision you made three weeks ago — and it presents as "the app does not
work" with no further detail.

The mitigations are correspondingly unglamorous. Fail early in `preflight`. Make
the tag name mean something and check it. Verify the artifact by unzipping it
rather than trusting the build. Read the documentation you are about to compile
into the binary. Look at the crash reports from the last release before shipping
the next one.

None of that is interesting engineering, and all of it is the difference between
a project that ships and one that ships and then spends a week finding out what
it shipped.

---

*Anchors:* [tools/release-aok.sh](../../tools/release-aok.sh),
[.github/workflows/build-release-ipa.yml](../../.github/workflows/build-release-ipa.yml),
[.github/workflows/build-dev-ipa.yml](../../.github/workflows/build-dev-ipa.yml),
[fastlane/](../../fastlane), [AppStoreExportOptions.plist](../../AppStoreExportOptions.plist),
[iSHRelease.entitlements](../../iSHRelease.entitlements),
[app/iSH.xcconfig](../../app/iSH.xcconfig),
[docs/release-notes-since-iSH-AOK_549.md](../../docs/release-notes-since-iSH-AOK_549.md),
[opt/AOK/docs/](../../opt/AOK/docs), [fs/aok-docs.manifest](../../fs/aok-docs.manifest).

*Story:* an unsigned IPA installing with no App Group — because sideloaders read
the entitlements of the app they are re-signing to decide what to request, and a
build with `CODE_SIGNING_ALLOWED=NO` leaves them nothing to read.
