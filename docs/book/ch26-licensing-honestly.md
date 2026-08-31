# 26. Licensing, honestly

This is an architecture book and this is a chapter about software licences,
which needs a justification before it needs an argument.

Here it is: in this system, the licence determines **what is inside the binary**,
and it does so at build-configuration time, through the same mechanism that
decides which programs exist. It is not a policy document filed somewhere and
consulted before a release. It is `meson_options.txt`, and getting it wrong
produces a shipped artifact containing code that should not have been there.

That makes it a build-system fact, and build-system facts belong in the same
book as everything else about the build.

## 26.1 `link_whole`, or: the registry does not decide

Chapter 22 described the native-program registry — a table of names and function
pointers — and it would be reasonable to assume that removing an entry removes
the program.

It does not. `meson.build` folds the vendored archives in with `link_whole`,
which is exactly what it sounds like: every object in the archive is linked,
whether or not anything references it. That is necessary, because a native
program's entry point is reached through a function pointer in a table and the
linker cannot see that as a use.

The consequence was measured rather than assumed:

> **Removing the applet-table entry in `kernel/native.c` is not sufficient.**
> `meson.build` folds these archives in with `link_whole`, so the objects ship
> whether or not anything references them — measured, **144 bash and 35 readline
> objects remain** with the registry entry deleted.

So a build that has "removed bash" by deleting a line still distributes GPLv3
bash. Only the build option removes it, and `-Dnative_bash=disabled` leaves
bash, readline and GNU termcap out of the archive entirely — **0 objects,
verified with `ar t`**.

The general form is worth carrying to any project that vendors: *what is in the
binary* and *what the program can do* are different questions with different
answers, and only one of them is visible in the source.

## 26.2 The conflict

bash is GPLv3, along with its bundled readline and GNU termcap. iSH-AOK is
GPLv3 too. That sounds like it should be the end of the matter, and it is not,
because of where the app is distributed.

The App Store's Usage Rules impose terms on what a user may do with a
downloaded application — how many devices, what may be redistributed. GPL
section 6 forbids imposing "further restrictions" on the rights the licence
grants. The Free Software Foundation's position is that the two are
incompatible, and it is not a theoretical position:

- **GNU Go**, removed from the App Store in 2010.
- **VLC**, removed in 2011.

Both on the FSF's initiative, both on the further-restrictions argument. The FSF
has also stated that the analysis applies to all GPL versions, not only v3, so
the GPLv2 relicensing described in the Foreword does not resolve it.

## 26.3 What `LICENSE.IOS` can and cannot do

Upstream iSH's answer is the document the Foreword called a gift:

> The copyright holders of the iSH app do not wish this conflict to prevent the
> otherwise-compliant distribution of derived apps via the App Store. Therefore,
> we have committed not to pursue any license violation that results solely from
> the conflict between the GNU GPLv2 or v3 and the Apple App Store terms of
> service.

That is what makes iSH-AOK distributable at all. It is a promise by iSH's own
copyright holders about iSH's own code.

And it binds nobody else. The FSF holds bash's copyright. `LICENSE.IOS` cannot
waive anything on the FSF's behalf, and the FSF has twice done exactly what the
document promises not to do.

So compiling bash into the binary is not a licensing question that has been
answered. It is a licensing question that has been *made visible*, which is a
different and more honest thing.

## 26.4 The answer is a build option

```bash
meson setup build .                          # auto: on if deps/bash is present
meson setup build . -Dnative_bash=disabled   # no third-party GPL in the binary
meson setup build . -Dnative_bash=enabled    # fail if deps/bash is missing
```

Three values doing three different jobs.

`auto` is today's default behaviour: on when the submodule is checked out. It is
the convenient answer and the one that makes a `--recurse-submodules` clone
produce a working bash.

`disabled` is the answer for anyone distributing a build they do not want to
carry third-party GPL code in.

`enabled` exists for a subtler reason, and it is the one worth copying. With
`auto`, a missing submodule silently produces a build without bash — which is
fine until it is a release build, and the release notes say bash is included,
and nobody notices until a user does. `enabled` turns that into a configure-time
failure.

The configuration output states which one you got:

```
Licensing
  native bash: no -- no third-party GPL in the binary
```

That line exists because "check rather than assume" is not advice that survives
contact with a release day. A build either prints that it contains third-party
GPL code or prints that it does not, in the same place every other configuration
decision is printed.

## 26.5 Users still get bash

The mere-aggregation position matters here and is easy to miss.

A build with `-Dnative_bash=disabled` still gives its users bash — the emulated
`/bin/bash` from the guest rootfs, installed by `apk` or `apt` like any other
package. That is exactly the same position every other GPL tool in Alpine or
Devuan occupies: distributed *alongside* the app, not compiled into it, which is
the arrangement the App Store has never had a problem with because it is the
arrangement every Linux distribution image on every platform uses.

What the build option changes is whether GPL code is *inside the binary Apple
signs*. What the user can run is unaffected. The only thing lost is the 16x, and
only for the shell.

## 26.6 Two other programs, two different answers

**zsh is on by default, and licensing is not why.** zsh's licence is permissive —
MIT-like — and none of its compiled C is GPL. Three GPLv2 files do exist in the
submodule (`Completion/Linux/Command/_qdbus`,
`Completion/openSUSE/Command/_osc`, `Completion/openSUSE/Command/_zypper`), and
the option's comment disposes of them precisely: they are completion **scripts**,
data rather than code, and nothing in this build compiles or installs them.

That distinction is worth making carefully rather than waving away. "There are
GPL files in the tree" and "GPL code is in the binary" are different claims, and
the answer to the first one is a list of three paths and an explanation.

**helix is off by default, and licensing is only half the reason.** helix is
MPL-2.0, which is file-level copyleft rather than GPLv3's reach — a materially
weaker obligation, and not the same conflict. The option's comment says the
decision anyway:

> it is still a deliberate decision about what the shipped binary contains and
> not one to make by default.

The other half is size. helix links tens of megabytes against an app binary that
is currently fourteen, and "a build that does not want an editor should not carry
one".

Two default-off options for two different reasons, each written down where the
option is defined. Somebody reading `meson_options.txt` in a year gets the
reasoning, not just the flag.

## 26.7 The rest of the inventory

Stated plainly, because a reader deserves the whole picture rather than the one
awkward part:

| component | licence |
|---|---|
| SmallCLUE | MIT |
| OpenSSH | BSD |
| libarchive | BSD |
| liblzma | public domain |
| zsh | permissive (MIT-like) |
| helix | MPL-2.0 (build option, default off) |
| bash, readline, GNU termcap | **GPLv3** (build option) |
| iSH-AOK itself | GPLv3, plus GPLv2 for post-relicensing contributions |

Nothing else in the binary is third-party GPL. That sentence is checkable, and
it is checkable in the way this book keeps recommending: `ar t` on the archive,
and the allowlist gate of Chapter 23 for what those objects reference.

## 26.8 The principle

A licence is a constraint on an artifact. In a project that vendors other
people's code and ships a signed binary, the artifact is decided by the build
system — so the licence has to live in the build system too, as an option with a
default, a failure mode, and a line of output saying which way it went.

Everything else — a note in a README, a policy someone remembers, a check on a
release checklist — is a constraint that is not enforced, and Chapter 37 is
about how many of those survive a release day.

The three things this chapter's implementation gets right, and that are worth
stealing:

**The removal is verified, not asserted.** `ar t`, 0 objects, and the number 144
recorded for what happens if you do it the wrong way.

**The build says what it did.** A `Licensing` heading in configure output, every
time, whether or not anyone reads it.

**The reasoning lives beside the switch.** Not in a document, not in a commit
message, not in somebody's memory. In `meson_options.txt`, next to the option,
where the next person to consider changing the default will be standing.

---

*Anchors:* [meson_options.txt](../../meson_options.txt) (the `native_bash`,
`native_zsh` and `native_helix` comments), [meson.build](../../meson.build)
(`link_whole`), [LICENSE.md](../../LICENSE.md), [LICENSE.IOS](../../LICENSE.IOS),
[README.md](../../README.md) ("Native bash and licensing"),
[kernel/native.c](../../kernel/native.c),
the FSF's writeups on the GNU Go and VLC removals.
