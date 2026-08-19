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
/AOK/README             one-line pointer to this filesystem
/AOK/version             build identifier
/AOK/docs/                this documentation set
/AOK/tools/               scripts and utilities (mount-root.sh, ktop, benchmarks, provisioning)
/AOK/tests/               the guest-side regression suite
/AOK/fixes/               canned fixes for known upstream-distro bugs
/AOK/persist/             writable, shared, survives everything (see persist.md)
/AOK/roots/               other installed roots, exposed read-write (see roots.md)
```

## Where the content actually comes from

`/AOK/docs`, `/AOK/tools`, and `/AOK/tests` are generated from plain files
in the iSH-AOK git repository, not copied onto your device at runtime:

- Doc sources live under `opt/AOK/docs/` in the repo (this file included).
- Tool sources live under `opt/AOK/tools/`.
- Test sources live under `tests/manual/`.

Three manifest files (`fs/aok-docs.manifest`, `fs/aok-tools.manifest`,
`fs/aok-tests.manifest`) list exactly which files from those directories
get shipped. At build time, `tools/gen-aokfs.py` reads each manifest and
embeds the listed files' contents directly into the compiled emulator as C
string tables, which `fs/aok.c` then serves at `/AOK/docs`, `/AOK/tools`,
and `/AOK/tests`. There is no on-device copy step — the bytes you're
reading right now were compiled into the app binary.

A couple of things fall out of that:

- `/AOK` is genuinely tiny and always available, even before any root is
  imported.
- If you're building iSH-AOK from source and want to see a new doc or tool
  show up under `/AOK`, it has to be listed in the matching manifest file,
  or it won't be embedded.

## A note on `/proc`, `/sys`, and `/dev`

iSH-AOK has no mount or PID namespaces — there's exactly one Linux kernel
underneath everything, so `/proc`, `/sys`, and `/dev` always reflect the
single true system state no matter which root or chroot you're looking at
them from. That's what makes tools like [ktop](ktop.md) useful from outside
a chroot: they see every process across every currently-mounted root,
labeled by guest architecture.
