# 21. `/AOK`: a filesystem compiled into the binary

Everything in Part IV so far belongs to a root. Install Devuan alongside your
Alpine and your tools are not there. Chroot into the riscv64 userland and your
tests are not there. Delete a root to reinstall it cleanly and your notes go with
it. And a guest that has broken its own `/etc` — which is a thing that happens,
because people experiment — has no way to reach a rescue script, because the
rescue script lived in the filesystem that is now broken.

`/AOK` is the answer to all of those at once: a filesystem that is not part of
any root, cannot be deleted from inside one, and is present on every boot before
anything else exists.

## 21.1 What it is

`aokfs` (`fs/aok.c`, magic `0x414f4b31` — "AOK1") is a synthetic read-only
filesystem mounted at `/AOK` at boot, regardless of which root filesystem is
running. Because it is not part of any root, it looks the same from Alpine,
Devuan, Arch, an i386 guest or a riscv64 one, and from inside a chroot into any
of them.

```
/AOK/README.txt      what this filesystem is, in a dozen lines
/AOK/VERSION         build identifier
/AOK/docs/           the in-app documentation set
/AOK/tools/          scripts and utilities
/AOK/tests/          the guest-side regression suite
/AOK/fixes/          canned fixes for known upstream-distro bugs
/AOK/native/         programs compiled into the app
/AOK/persist/        writable, host-backed, survives everything
/AOK/fakefs/         writable, survives everything, keeps full Linux metadata
/AOK/roots/          your other installed roots, read-write
```

## 21.2 The bytes are in the binary

The unusual part is where the content comes from. `/AOK/docs`, `/AOK/tools` and
`/AOK/tests` are not copied onto the device at install time and they are not
extracted on first boot. They are compiled into the application.

Three manifest files list what ships:

```
fs/aok-docs.manifest     27 lines
fs/aok-tools.manifest    32 lines
fs/aok-tests.manifest   210 lines
fs/aok-libs.manifest    334 lines   (grammars and support files for helix)
```

At build time `tools/gen-aokfs.py` reads each manifest, reads the listed files
out of the source tree, and emits a C include full of string tables:

```c
struct aokfs_gen_file { const char *path; const char *data;
                        unsigned size; unsigned mode; };
```

`fs/aok.c` then serves those tables as a filesystem. The embedding is
**byte-exact** — the generator's docstring is explicit that size is the real
byte count rather than `strlen`, which is what lets binary content through
unmangled.

Two consequences fall out of that, and one trap.

`/AOK` is **genuinely tiny and always available**. There is no unpacking step to
fail, no disk space to run out of, and it works before any root has been
imported at all — which is exactly the state a new user is in, and exactly the
state a user who has just deleted a broken root is in.

It is also **impossible for a guest to damage**. A `rm -rf /` inside a root
cannot touch it, because there is nothing on disk to remove; the mount would
simply be there again on the next boot.

And the trap, which catches every new contributor once: **a file not listed in a
manifest is silently absent on the device.** It does not fail to build. It does
not produce a warning. It is simply not there when you go looking, on a device,
after a twenty-minute build. Chapter 9 mentioned the same hazard for tests,
which need registering in three places across two files; this is where the
quietest of the three lives.

## 21.3 `/AOK/fixes`, and where a workaround should live

`/AOK/fixes` is a small directory with a specific philosophy behind it. When a
distribution ships something that does not work under AOK — Devuan's
`pkcsslotd` init script, an Arch packaging assumption, a Debian symlink — the
fix has to live somewhere.

The obvious place is the root filesystem image, patched before it ships. That
means the fix is frozen at image-build time, applies only to roots created
afterwards, and is invisible to anyone who imported their own tarball.

Putting it in `/AOK/fixes` inverts all three. The fix travels with the
*application*, so an app update fixes existing roots; it is the same fix for
every root of that distribution; and it is discoverable, because it is a file
with a README next to it rather than a diff somebody applied once.

## 21.4 Three ways to have something writable

Three entries under `/AOK` break the read-only rule, and choosing between them
is a real decision rather than a formality.

**`/AOK/persist`** is a real host directory in the app's shared App Group
container. It survives root switches, app updates and reinstalls, because it is
not inside any root's data store nor inside the app's own sandbox. It is
directly host-backed, so access through it has no emulated-filesystem
translation at all — which is why the things the *app itself* reads live there:
downloaded root archives, the music library, the LLM chat log. `/AOK/persist/bin`
is first on the default `PATH`, which makes it the natural home for your own
programs.

The cost is stated plainly in the in-app documentation: being host-backed, it
**flattens Linux ownership and cannot hold device nodes**.

**`/AOK/fakefs`** survives exactly the same things, and is backed by a
filesystem of the kind an installed root uses — so it keeps uid, gid, modes,
device nodes and hardlinks. It is where a cross-root tree that needs real Linux
semantics goes; a `debootstrap`'d rootfs, for instance, which `/AOK/persist`
simply cannot hold.

Those two are Chapter 17's trade offered as a user-facing choice. The host store
is fast and lossy. The SQLite-backed store is slower and faithful. Both outlive
everything else. You pick by whether you need Linux metadata, and the
documentation says so in those terms rather than making you infer it.

**`/AOK/roots`** exposes every *other* installed root, read-write, so you can
`chroot` from an arm64 Alpine into an x86_64 Devuan without leaving the guest.
Chapter 30 covers what that is for.

## 21.5 `/AOK/native` has no manifest

The last directory works differently from all of them. There is no manifest,
because there are no files: each entry corresponds to one program compiled into
the application and registered in `kernel/native.c`, and `execve` of the path
runs that host code instead of loading an image (Part V).

The design decision worth naming is what happens for a program this build does
not carry:

> A program this build does not carry has **no entry at all**, rather than an
> entry that fails.

`ls /AOK/native` is therefore an accurate inventory of what is inside this
particular binary. A build without helix has no `hx`; a build configured with
`-Dnative_bash=disabled` (Chapter 26) has no `bash`. Nothing is listed that
would produce "not implemented" when run.

That is the capability-honesty principle of Chapter 40 expressed as a directory
listing, and it is a small thing that saves a specific kind of confusion: the
user who runs a program, gets an error, and cannot tell whether they have
misconfigured something or whether the feature was never compiled in.

## 21.6 The advantage hiding inside a limitation

Chapter 10 established that AOK has no PID or mount namespaces, and treated it
as a limitation — nothing container-shaped works, and that is architectural.

`/AOK` is where the same fact reads as an advantage, and the in-app overview
states it directly:

> iSH-AOK has no mount or PID namespaces — there's exactly one Linux kernel
> underneath everything, so `/proc`, `/sys`, and `/dev` always reflect the
> single true system state no matter which root or chroot you're looking at them
> from.

So a process viewer running inside a chroot sees *every* process on the system,
across every mounted root, labelled by guest architecture. `ktop` — the tool in
`/AOK/tools` built for exactly this — is useful from anywhere precisely because
there is nowhere to hide.

On a real Linux box that would be a security failure. Here there is one user,
one trust domain, and one kernel, and the property that would be a container
escape elsewhere is simply visibility.

It is worth being precise that this is not a second design decision. It is the
same absence of namespaces, seen from the other side, and the honest summary is
that the project traded isolation it could not implement for a global view it
did not have to build.

## 21.7 The pattern: making the app addressable

Put `/AOK` beside `/proc/ish` (Chapter 18) and a shared idea appears.

An application has state and it has assets. A terminal application's users are
people who work in a shell. So the app's settings become files under
`/proc/ish/defaults`, readable with `cat` and writable with `echo`; and the
app's documentation, tools, tests and compiled-in programs become files under
`/AOK`, readable with `less` and runnable with `sh`.

Neither is a filesystem in the sense of Chapter 17 — nothing is stored, and the
underlying state is a preference database in one case and a table of string
literals in the other. Both exist because the alternative is a feature that can
only be reached by tapping a control, in a product whose entire purpose is not
having to.

## 21.8 Closing Part IV

Six chapters, and a single question underneath all of them: what does a file
mean when there is no filesystem?

fakefs answered it with a database, because the sandbox cannot express
ownership. The synthetic filesystems answered it with generated text, because
`/proc` was never storage in the first place. Sockets answered it by forwarding
to a host with different opinions. FUSE answered it by letting a guest program
answer instead. And `/AOK` answered it with string tables in the binary, because
some files should not belong to any root at all.

What they have in common is that none of them stores what the guest thinks it
stores, and all of them have to be indistinguishable from something that does.

Part V is the sharpest form of that same trick, applied one level up: a program
that is not a program.

---

*Anchors:* [fs/aok.c](../../fs/aok.c), [fs/aok-docs.manifest](../../fs/aok-docs.manifest),
[fs/aok-tools.manifest](../../fs/aok-tools.manifest),
[fs/aok-tests.manifest](../../fs/aok-tests.manifest),
[fs/aok-libs.manifest](../../fs/aok-libs.manifest),
[tools/gen-aokfs.py](../../tools/gen-aokfs.py),
[kernel/native.c](../../kernel/native.c),
[opt/AOK/docs/00-overview.md](../../opt/AOK/docs/00-overview.md),
[opt/AOK/docs/persist.md](../../opt/AOK/docs/persist.md),
[opt/AOK/docs/roots.md](../../opt/AOK/docs/roots.md),
[opt/AOK/docs/ktop.md](../../opt/AOK/docs/ktop.md).
