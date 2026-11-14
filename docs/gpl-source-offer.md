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
3. ~~**No per-release pinning.**~~ **Fixed 2026-11-14.**
   `tools/root-manifest.py` reads a shipped image's package database straight
   out of the tarball and writes a manifest; `docs/root-manifests/` holds one
   for each image we currently ship. Each records the image's **SHA-256**, the
   **archive and suite** the packages came from, and every package with its
   **version and source-package name** -- which is exactly what a source
   request needs:

   | | |
   |---|---|
   | [alpine-minirootfs-3.24.2-aarch64](root-manifests/alpine-minirootfs-3.24.2-aarch64.md) | 16 packages, with licences |
   | [devuan-minirootfs-6.0-aarch64](root-manifests/devuan-minirootfs-6.0-aarch64.md) | 96 packages, `deb.devuan.org/merged excalibur` |

   Run it for every image before a release and commit the result:

   ```
   tools/root-manifest.py *-minirootfs-*.tar.* -o docs/root-manifests
   ```

   The SHA-256 is the part that makes this worth anything: it ties a manifest
   to one exact artifact, so "the bash in build 565" stops being a question of
   memory. Alpine's database carries a licence per package; Debian's does not,
   so the manifest names the source package to fetch instead.

Items 1 and 2 remain. They are decisions, below.

---

## The decisions

1. **Keep the Devuan root bundled, or download it?** The assessment's option
   was to have App Store builds fetch it from the catalogue instead (1-2 days).
   That does not remove the obligation -- we would still be distributing it --
   but it makes the shipped app smaller and the conveyance a network one.
2. ~~**Where the source lives.**~~ **Decided 2026-11-14: a per-release
   snapshot of the source packages.** `tools/fetch-root-source.py` produces
   it:

   ```
   tools/fetch-root-source.py devuan-minirootfs-6.0-aarch64.tar.xz --dry-run
   tools/fetch-root-source.py devuan-minirootfs-6.0-aarch64.tar.xz -o source/
   ```

   For the Devuan image that is **66 source packages, 212 files, 340 MB** --
   every binary package in it resolved, none missing. That is the real number
   to plan hosting around, and it is a GitHub release asset rather than
   anything we have to run.

   Resolution is exact rather than constructed. The tool reads the archive's
   own `Sources` index, which gives each source package's version, its pool
   directory and a SHA-256 per file, and verifies every download against it.
   Guessing a URL from a binary version would have been wrong twice over: a
   binNMU (`bash` is `5.2.37-2+b10` as a binary and `5.2.37-2` as source) and
   anything carrying an epoch. It also handles the merged archive splitting
   into `pool/DEBIAN/` and `pool/DEVUAN/`, which no constructed URL would.

   Alpine is a different shape and the tool says so instead of pretending: an
   apk's corresponding source is its APKBUILD in aports plus the upstream
   tarball it fetches, so what gets recorded is the aports clone and the
   `v3.24.2` tag.

   Checked on the real archive: all 66 resolved, and three files fetched and
   checksum-matched against the index.
3. ~~**Where the offer text goes.**~~ **Decided 2026-11-14: a Legal row in
   Settings**, beside Privacy Policy. Built, and the text below is what it
   shows.

   Pushed as a screen rather than opened in Safari, which matters more than it
   looks: the GPL requires the written offer to **accompany** the binaries it
   covers. A link to a website does not accompany anything, and an offer that
   needs the network to be read is no use to someone on a plane with the app
   they were given. It also names the build number it is shown in, so a
   request can say which one it is about.

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

`tools/root-manifest.py` does it, with no network and nothing installed -- the
package databases are plain text inside the archives (`lib/apk/db/installed`
for Alpine, `var/lib/dpkg/status` for Devuan), and it reads them without
extracting the image.

```
tools/root-manifest.py *-minirootfs-*.tar.* -o docs/root-manifests
```

**This belongs in the pre-release pass**, next to the README and `/AOK/docs`
audit. Run it whenever an image changes and commit the manifest; it is cheap
now and unrecoverable later.
