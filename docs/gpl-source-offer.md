# The GPL source offer

iSH-AOK distributes a lot of other people's copyleft software. This is the
working document for what that obliges us to do, what we have not done yet,
and the text to use once the open decisions are made.

**Not legal advice.** It is an engineer's reading of the licences, written so
the decisions are made deliberately rather than discovered at submission.

---

## First, what we do NOT owe anything for

A photo app that displays your pictures owes nobody a copyright notice. The
developer never copied or distributed those pictures; the app is a tool that
opens a file somebody else supplied.

**Almost everything iSH-AOK runs is exactly that case.** When a user types
`apt install` or `apk add`, the packages travel from Devuan's or Alpine's
servers to that user's device. We did not convey them, we are not a party to
it, and no source obligation of ours attaches -- however much GPL software
ends up in their filesystem afterwards. The same goes for anything they
compile, download or copy in themselves. This is also the 2.5.2 argument in
the review notes: a guest binary stands to the emulator as a document stands
to the app that opens it.

What follows is only about the things **we** put in front of users.

One thing that follows from this, worth being clear about because README.md
gets it half right: "mere aggregation" protects AOK's *own* source from being
swept into the GPL by sitting next to GPL software. It says nothing about the
obligation to provide source for the GPL software we hand out. Those are
different questions, and only the second one is open.

---

## What we actually ship

Three separate things, with different obligations.

### 1. The bundled root filesystems

Both are inside the app bundle, so they are conveyed to every user:

| | |
|---|---|
| `alpine-minirootfs-3.24.2-aarch64.tar.xz` | 3.2 MB, 16 packages |
| `devuan-minirootfs-6.0-aarch64.tar.xz` | 32.5 MB, 96 packages |

Alpine's package database records a licence per package, so its position is
exact. **Nine of the sixteen are GPL:**

| Package | Version | Licence |
|---|---|---|
| alpine-baselayout | 3.7.2-r1 | GPL-2.0-only |
| alpine-baselayout-data | 3.7.2-r1 | GPL-2.0-only |
| apk-tools | 3.0.8-r0 | GPL-2.0-only |
| busybox | 1.37.0-r31 | GPL-2.0-only |
| busybox-binsh | 1.37.0-r31 | GPL-2.0-only |
| libapk | 3.0.8-r0 | GPL-2.0-only |
| musl-utils | 1.2.6-r2 | MIT AND BSD-2-Clause AND GPL-2.0-or-later |
| scanelf | 1.3.9-r1 | GPL-2.0-only |
| ssl_client | 1.37.0-r31 | GPL-2.0-only |

The rest are MIT, Apache-2.0, Zlib, or `MPL-2.0 AND MIT`
(ca-certificates-bundle).

The Devuan root is the larger exposure: 96 Debian packages, including **GPLv3
bash, coreutils, grep, findutils, diffutils, gzip and dpkg**, and GPLv2 apt,
dash and busybox-static. This is the root README.md's objection applied to
itself -- native bash is kept out over the FSF's position on the App Store
terms, and a GPLv3 bash inside a tarball we ship is the same bash.

### 2. Copyleft compiled into the app binary

- **OpenMinis/ish-arm64 (GPLv3)** -- `jit/guest-arm64/*.S`, `emu/arm64_interp.c`,
  `kernel/calls.c:1807`; see docs/CREDITS-aarch64.md. Their repository carries
  a LICENSE.IOS and they ship on the App Store themselves.
- **libgit2 (GPLv2 with a linking exception)** -- the exception is what makes
  this fine, and it is worth not losing track of.

### 3. MPL-2.0, via helix

Seven crates: `nucleo`, `nucleo-matcher`, `smartstring`, `termina`,
`tree-house`, `tree-house-bindings`, `uluru`. Checked 2026-11-14; there is no
GPL anywhere in helix's dependency tree. MPL-2.0 is file-level copyleft and we
ship these unmodified, so saying where they came from discharges it. Fold them
into the same offer rather than writing a second one.

---

## What the licences require

Plainly, and only the part that bites us:

- **GPLv2 section 3.** Distributing binaries means either shipping the
  complete corresponding source with them, or including **a written offer,
  valid for at least three years**, to give any third party that source for no
  more than the cost of distribution. The third option -- passing along the
  offer you received -- is for noncommercial distribution only, so it is not
  available here.
- **GPLv3 section 6.** The same shape. Accompany with source, or a written
  offer valid for at least three years. Section 6(d)'s "point at a server"
  route is for object code offered *from* a network server, and we do not
  control Apple's.
- **MPL-2.0 section 3.2.** Source for the covered files must be available to
  recipients. Unmodified upstream crates, so a pointer suffices.

So: **a written offer, in the app, valid three years from each release.**

The hard part is not the text. It is being able to honour it: the offer is
worthless unless we can actually hand someone the exact source for the exact
binaries in a given build, years later.

---

## What we have not done

1. **No offer accompanies the app.** There is nothing in the About screen, the
   App Store description or README.md.
2. **No source archive exists.** If someone asked today for the source
   corresponding to the bash in build 565's Devuan root, we would be
   reconstructing it from memory of which Devuan snapshot that root was built
   from.
3. **No per-release pinning.** The roots are built by the rootfs tooling; the
   exact upstream package versions that went into a shipped tarball are not
   recorded anywhere in this repository.

Item 3 is the one to fix first, because it is the one that gets harder with
time. The package lists in this document were read back out of the shipped
tarballs, which is evidence that it is recoverable -- see the commands below.

---

## The decisions

1. **Keep the Devuan root bundled, or download it?** The assessment's option
   was to have App Store builds fetch it from the catalogue instead (1-2 days).
   That does not remove the obligation -- we would still be distributing it --
   but it makes the shipped app smaller and the conveyance a network one.
2. **Where the source lives.** Options, cheapest first:
   - a dated snapshot of the `.deb`/`.apk` *source* packages for each release,
     kept as a GitHub release asset on a separate repository;
   - a documented, reproducible recipe plus pinned versions, with the actual
     tarballs produced on request;
   - mirroring Devuan's and Alpine's source archives, which is the most
     complete and by far the most expensive.
3. **Where the offer text goes.** At minimum a Legal row in Settings, beside
   Privacy Policy. The App Store description is the other obvious place.

---

## Draft offer text

For a Legal screen in the app and for README.md. Fill in the URL and the
address once decision 2 is made.

> **Source code for the GNU/Linux software included with iSH-AOK**
>
> iSH-AOK includes Linux filesystem images containing software licensed under
> the GNU General Public License, versions 2 and 3, among other licences --
> including GNU bash, coreutils, grep, findutils, diffutils, gzip, dpkg and
> apt in the Devuan image, and busybox, apk-tools and scanelf in the Alpine
> image. It also includes code from the ish-arm64 project under the GPL
> version 3, and crates under the Mozilla Public License 2.0.
>
> For any version of iSH-AOK, for at least three years after that version was
> distributed, we will give any third party the complete corresponding source
> code for the GPL-licensed software in it, on request, for no more than our
> cost of physically performing the distribution. The same source is also
> available at [URL].
>
> Write to [ADDRESS], or open an issue at
> https://github.com/emkey1/ish-AOK/issues, naming the iSH-AOK build number
> you have.

---

## Regenerating the inventory

Both lists in this document came out of the shipped tarballs directly, with no
network and nothing installed. The databases are plain text inside the
archives:

- Alpine: `lib/apk/db/installed` -- `P:` name, `V:` version, `L:` licence.
- Devuan: `var/lib/dpkg/status` -- RFC822 blocks with `Package:` and
  `Version:`. Debian's status file carries no licence field; the licence is in
  `/usr/share/doc/<package>/copyright` in the full package, so a licence
  column for Devuan means reading those or the source packages.

Do this again for each release, and keep the output with the build. That alone
turns decision 2 from an archaeology problem into a lookup.
