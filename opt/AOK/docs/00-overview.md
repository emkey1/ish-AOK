# /AOK: what it is and how it's built

`/AOK` is not a copy of files sitting on disk inside your Linux root. It's a
small, synthetic, read-only filesystem (`aokfs`, see `fs/aok.c`) that
iSH-AOK mounts at `/AOK` on every boot, regardless of which root filesystem
(Alpine, Devuan, i386/amd64/arm64/riscv64) you're currently running. Because
it isn't part of any root, it shows up the same way no matter which
filesystem you booted or chrooted into, and it can't be deleted or corrupted
by anything that happens inside a guest root.

Three entries under `/AOK` break the "read-only" rule and are real, writable
mounts instead of synthetic ones:

- `/AOK/persist` — a single writable location, backed by a real host
  directory, that survives root switches, app updates, and reinstalls. Being
  host-backed, it flattens Linux ownership and cannot hold device nodes. See
  [persist.md](persist.md).
- `/AOK/fakefs` — a second writable location surviving the same things, but
  backed by a filesystem of the kind an installed root uses, so it keeps full
  Linux metadata: uid/gid, modes, device nodes and hardlinks. Use it for a
  cross-root tree that needs real filesystem semantics — a debootstrap'd
  rootfs, say — which `/AOK/persist` cannot hold.
- `/AOK/roots` — read-write views of your *other* installed root
  filesystems, used for chrooting between them. See [roots.md](roots.md).

Everything else under `/AOK` is baked into the app at build time:

```
/AOK/README.txt           what this filesystem is, in a dozen lines
/AOK/VERSION              build identifier
/AOK/docs/                this documentation set
/AOK/tools/               scripts and utilities (native-links.sh, persist-links.sh,
                          manage-roots.sh, mount-root.sh, ktop, benchmarks,
                          provisioning, Wayland)
/AOK/tests/               the guest-side regression suite
/AOK/fixes/               canned fixes for known upstream-distro bugs
/AOK/native/              programs compiled into the app -- exec'ing one runs host
                          code instead of translated guest code (native-programs.md)
/AOK/persist/             writable, host-backed, survives everything (see persist.md)
/AOK/fakefs/              writable, survives everything, keeps full Linux metadata
/AOK/roots/               other installed roots, exposed read-write (see roots.md)
```

## Where the content actually comes from

`/AOK/docs`, `/AOK/tools`, `/AOK/tests` and `/AOK/native/libs` are generated
from plain files in the iSH-AOK git repository, not copied onto your device
at runtime:

- Doc sources live under `opt/AOK/docs/` in the repo (this file included).
- Tool sources live under `opt/AOK/tools/`.
- Test sources live under `tests/manual/`.
- Native-program support files live under `deps/helix/runtime/`, and are
  served at `/AOK/native/libs`.

Four manifest files (`fs/aok-docs.manifest`, `fs/aok-tools.manifest`,
`fs/aok-tests.manifest`, and `fs/aok-libs.manifest` for `/AOK/native/libs`)
list exactly which files get shipped. At build time, `tools/gen-aokfs.py`
reads each manifest and embeds the listed files' contents directly into the
compiled emulator as C string tables, which `fs/aok.c` then serves at
`/AOK/docs`, `/AOK/tools`, `/AOK/tests` and `/AOK/native/libs`. There is no
on-device copy step — the bytes you're reading right now were compiled into
the app binary.

A couple of things fall out of that:

- `/AOK` is genuinely tiny and always available, even before any root is
  imported.
- If you're building iSH-AOK from source and want to see a new doc or tool
  show up under `/AOK`, it has to be listed in the matching manifest file,
  or it won't be embedded.

`/AOK/native` is different again: its program entries have no manifest. Each is
one program compiled into the app and registered in `kernel/native.c`, and
`execve` of the path runs that host code instead of loading a guest image. A
program this build does not carry has no entry at all, rather than an entry that
fails. See [native-programs.md](native-programs.md) and
[native-setup.md](native-setup.md).

The one exception is `/AOK/native/libs` — support files a native program reads
at runtime, such as helix's tree-sitter queries and themes. Those *are*
manifest-driven, from `fs/aok-libs.manifest`, and unlike the other manifests its
paths may nest arbitrarily deep: the directories under `/AOK/native/libs` are
derived from the listed paths rather than declared in `fs/aok.c`.

## A note on `/proc`, `/sys`, and `/dev`

iSH-AOK has no mount or PID namespaces — there's exactly one Linux kernel
underneath everything, so `/proc`, `/sys`, and `/dev` always reflect the
single true system state no matter which root or chroot you're looking at
them from. That's what makes tools like [ktop](ktop.md) useful from outside
a chroot: they see every process across every currently-mounted root,
labeled by guest architecture.
