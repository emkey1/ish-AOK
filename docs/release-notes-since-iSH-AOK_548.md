# Release Notes Since `builds/iSH-AOK_547`

92 commits. Two things dominate: a performance cycle that made every guest
engine measurably faster, and a filesystem cycle that found four ways for two
different files to be mistaken for one. The performance work is closed and
documented; the filesystem work started with a report of three files in three
different roots all claiming `dev=265 ino=308`.

Several of these were found by testing the *disabled* arm of a switch, or by
taking a symptom seriously that looked like it belonged to something else. Two
were found only because a stale toolchain was deleted.

## Highlights

**Files in different roots could be mistaken for the same file.** fakefs
numbers its own inodes out of SQLite but built the rest of the statbuf by
calling through to realfs, so `st_ino` came from the database while `st_dev`
came from whatever host volume the backing store sat on. Every fakefs mount on
one volume therefore reported the same device while numbering inodes
independently from 1. Userland treats an equal `(dev, ino)` pair as "same file"
and skips the work, so `cp` between two roots refused with "are the same file"
having copied nothing. Every fakefs database numbers its root directory inode
1, so the root directories of any two mounts were literally the same file.
`a3ea924e`.

**One file's metadata could be attached to another's path.** `path_create`
allocated inodes from an in-memory counter seeded once from `max(stats.inode)`.
A second allocator on the same `meta.db` -- another `ish` process on the same
root, or that root mounted twice -- seeded from the same watermark and handed
out numbers the first was still using. The failing `insert into stats` was only
`printk`ed, so the `insert or replace into paths` below it ran anyway and left
a path pointing at another file's stat blob. Invisible until the two disagreed
about being a directory, at which point the name could not be removed by
anything: `rmdir` got ENOTDIR from the host, `unlink` EISDIR from the metadata.
A real root reached "Cannot create temporary file in /tmp/: Is a directory" and
could not build anything. `a1de4b3e`, with the repair pass in `4956f332`,
`f95e85c2` and `f43bddfe`.

**The guest engines got faster across the board.** Return caches for the
aarch64 and riscv64 guests (`d8e8a306`, `e35caf33`), which had been exiting to
C on every guest return; i386 instruction fusions for address computation, ALU,
mov, LEA and push/pop; the riscv64 call fold; and the removal of a ~40 KB
`memset` that ran on every single guest syscall (`711b48f3`, -27.2%). The
measurements and method are in `docs/perf_benchmarks_2026_08.md`; the engine is
dispatch-bound at ~6.8 ns/dispatch, which is what makes fusion pay.

**An i386 image inherited a 64-bit ancestor's fault address.** `segfault_addr`
is a 64-bit field shared by every guest ABI, but the i386 JIT wrote it with a
32-bit store, and nothing cleared the high half across an exec that changes
ABI. Every statically linked i386 binary died in libc startup when run from an
aarch64 root, while the same binary exec'd in place worked. It looked
filesystem-specific and was not. `536e65c8`.

**An exec out of a shared mm kept running the old program's translations.**
The per-host-thread JIT block memo is keyed by guest address alone, and execve
replaces the task's mm and jit while the host thread lives on. An ordinary exec
was saved by the free counter; a *shared* mm was not, which is why `dash -c
uname` crashed while `exec uname` did not. `a7769162`.

## User-Facing Changes

### Emulation

- Return caches for the aarch64 and riscv64 guests, and i386 fusions for
  address computation, ALU, mov, LEA and push/pop. `d8e8a306`, `e35caf33`,
  `1edd8e0f`, `eb83bdf0`, `a8cc7f5f`, `52d4bbff`, `7d9a27c3`, `d2bae733`.
- The ~40 KB of JIT scratch zeroed on every guest syscall is gone, in every
  engine. `711b48f3`.
- amd64 register inc/dec is a native gadget rather than a C helper that
  re-decoded the instruction. `669c4c49`.
- The x86_64 host lost the flags of every locked ALU operation to the compare
  and swap that followed it. `97886877`.
- i386 `pread`/`pwrite` silently truncated the 64-bit offset to 32 bits.
  `914a30ec`.
- Guest dispatch on arm64 and riscv64 defaults to `dmb`, 1.73x on ARMv8.0.
  `7e3d8246`.

### Filesystem and kernel

- `st_dev` and `st_ino` now come from one namespace, so files in different
  roots are different files. `a3ea924e`, with `mount_cross_dev` as the guard.
- Inode aliasing is fixed and existing damage repaired, including the
  case-twin duplicates a case-sensitive host left behind. `a1de4b3e`,
  `f95e85c2`, `f43bddfe`.
- `/proc/self/mountinfo` reports each mount's real device instead of a
  hardcoded `0:0`. `088ef7c0`.
- An empty cgroup can be removed. cgroup2 gives every directory interface
  files, and counting them as children made `rmdir` answer ENOTEMPTY for every
  cgroup in existence, so systemd could never clean one up. `d219f0d1`.
- `df` names filesystems instead of printing app-group container paths.
  `88496575`, `0a77a88c`, `cdfadd99`, and the field-shift fix in `14dd7660`.
- `statfs` on a bind mount reports the origin's superblock rather than failing
  against the bind's absent backing. `f493d8e3`.
- devtmpfs is a real filesystem instead of an unconditional no-op. `f258a7c1`.
- `RTM_GETLINK` ignores the request's family, so `ip -4 addr` and `ip -6 addr`
  work. `7cdf507b`, `74e33729`.

### App

- The Filesystems list shows where each root is mounted, and shows nothing when
  a root failed to mount, which is the only way to tell a broken entry from a
  healthy one. The detail screen also gained a way back out: it was reached by
  a `show` segue with no navigation stack, so it presented modally and delete
  or export were the only exits. `785bdf5d`.
- Importing a file into the Files provider reports an error instead of aborting
  the extension. A shipped `assert` turned "the source is a directory" and "the
  source no longer resolves" into a crash. `5d4962a9`.
- Stale mount-point directories under `/AOK/roots` are actually pruned, and the
  error is no longer discarded. They had been accumulating for weeks and were
  indistinguishable from real roots. `baf2c61d`.
- Multiple LLM chat sessions with switchable destinations, and streaming from
  https providers. `f900f470`, `aa1ed968`, `e2e3f7e2`, `55dcae5a`, `145d612c`.
- Terminal scrollback search. `5fe00a09`.

## Validation

`meson test` green on both hosts: the aarch64 host reports 2 OK with float80
SKIPped by design, and the x86_64 oracle reports 3 OK including float80 at
1224/1224.

The full guest suite (~105 tests) passes on all four guest architectures on the
CLI rig: i386 115, amd64 116, arm64 114, riscv64 107, zero failures. The only
skips are the two accelerators that exist solely in the app build, and
`bcd_adjust` on amd64, which is correct since those opcodes are invalid in
64-bit mode.

The curated release set was additionally run through **both arms of every
architecture's fusion switch**, with the mask read back before each arm. A
newly reachable fallback path is untested code, and gating the riscv64 JAL
fusion once introduced a miscompile visible only with the fusion off.

On device: an M4 iPad Pro ran the full suite on all four architectures, native
aarch64 clean at 115 passes; an A9 iPad ran the curated set on all four. The
`mount_stdev` and `devtmpfs_mount` failures seen in device chroots are an
artifact of the harness, not the build -- iSH has no mount namespaces, so
`/proc/self/mountinfo` is never rebased to a chroot root and those two tests
compare it against paths the chrooted process sees.

## Known Issues

- **Three terminfo entries in one migrated root come back with the wrong
  type.** The case-twin repair restores content but can invert an entry's
  symlink-ness: on one root `A/Apple_Terminal` is a regular file holding
  `../n/nsterm`, and two entries under `N` are symlinks that dangle, one with
  an 1850-byte target. Duplicates themselves are gone and the rest of the root
  is clean (1 implausible symlink in 3888).
- **The systemd user session is degraded on Arch.** `systemctl --user` cannot
  reach the user bus, and the gpg-agent, `keyboxd`, `p11-kit` and `machined`
  sockets do not come up. The cgroup layer is fine: delegation works and the
  user's cgroup is correctly owned.
- **`PrivateNetwork=yes` units cannot start.** `CLONE_NEWNET` is unimplemented,
  so `shadow`, `fstrim`, `mkinitcpio-generate-shutdown-ramfs` and
  `systemd-hostnamed` exit `225/NETWORK`.
- **A busy mount cannot be detached.** `umount -l` returns busy where Linux's
  MNT_DETACH always succeeds, and a sysfs mount stayed busy with no process
  rooted or cwd'd inside it.
- The `futex_core` flake carried forward from previous releases.

## Maintainer Notes

- Three build files each picked a dependency from `/usr/local` before
  `/opt/homebrew`, which on Apple silicon means preferring the Intel Homebrew:
  libarchive (`ba1cd8c5`), the VDSO compiler (`f1c2d91d`, which had been
  building the VDSO with a 2021 clang-12 under Rosetta), and unicorn
  (`c07b3d4e`). The unicorn one could not be found by inspecting the build
  graph, because `/usr/local/include` is an implicit clang search path and so
  wrote no flag; it surfaced only when the stale prefix was deleted.
- `tests/manual/jit_fuse_ab.sh` now restores the mask it changes on every exit
  path, and refuses a clock with no sub-second hand. It had been leaving
  `retcache off` on device, so later measurements silently ran against a
  crippled build. `6cb417fe`, `e571db7e`.
- The `deps/rootfs-manifest` submodule moved to https. It was the only one on
  an SSH URL, so any clone without a GitHub key failed on it alone.
  `2fc33c43`.
- float80 had been failing on both hosts for months and was neither a printing
  bug nor a float80 bug. `deconst_dummy` was a plain global, so the compiler
  folded the *reference* at compile time in round-to-nearest, invalidating
  every directed-rounding comparison. It now SKIPs on hosts whose `long double`
  is not the x87 80-bit format, since there is nothing to compare against
  there. `4e0712b0`.

## Commit Range

`builds/iSH-AOK_547..builds/iSH-AOK_548`

```
100d3d8b build: bump project version to 548
c07b3d4e tools: take unicorn's library and its header from the same prefix
f1c2d91d vdso: build with this machine's clang, not the leftover Intel one
ba1cd8c5 tools: find the libarchive built for this machine, not the first one on disk
d219f0d1 fs/tmp: an empty cgroup could not be removed, so none ever was
2708071e tests/fakefs_inode_alias.sh: read the migrated version instead of naming it
f43bddfe fs/fake-migrate: don't cap how many aliases one directory can have repaired
f95e85c2 fs/fake-migrate: a case-sensitive host left two spellings of one guest name
baf2c61d app: actually prune /AOK/roots, and stop discarding the error when it fails
785bdf5d app/Filesystems: show where each root is mounted, and a way back out
cdfadd99 app: name the rest of the /AOK mounts too, so the df column can shrink
0a77a88c app: name the secondary root mounts in df, like / already is
5aa87e4d tests/mount_cross_dev: on device the roots are already mounted, not mountable
2fc33c43 deps: the rootfs-manifest submodule was the only one gated behind SSH
5d4962a9 app/FileProvider: an import that cannot proceed is an error, not an abort
a1de4b3e fs/fake: an inode collision could alias one path onto another file's metadata
0fd2cc7e tests: syscall_wiring asserted three things a real kernel disagrees with
c2ef89a4 deps/rootfs-manifest: bump to the locale-patched Devuan 6 images
088ef7c0 fs/proc: mountinfo's device field, instead of a hardcoded 0:0
2d584c55 rootfs: the same locale patch for the two downloadable Devuan masters
91b3b5be rootfs: the bundled Devuan aarch64 image carries the locale files now
4e0712b0 emu/float80-test: the reference was computed by the compiler, in the wrong mode
e571db7e tests/jit_fuse_ab: busybox date drops %N, so every rep timed as zero
6cb417fe tests/jit_fuse_ab: put the mask back, on every exit path
a66ca389 rootfs: a Devuan image named no locale, so UTF-8 tools refused to run
a3ea924e fs/fake: an inode from SQLite paired with a device from the host volume
f493d8e3 fs: statfs on a bind ran against the bind's own absent backing
d51ac3d0 docs: the August 2026 performance measurements, with their method
14dd7660 proc/mounts: an empty source field shifted every later field by one
b6a6e110 fs/mount: the bind path builds its own mount, and missed the new field
88496575 fs: df named / by the host path it is stored at, not by the root
74e33729 Merge fix/netlink-getlink-family-filter: RTM_GETLINK must ignore the request family (ip -4/-6 addr)
805d7b7e perf: the aarch64 return cache on the A9 -- and the ordering it restores
4956f332 fs/fake-migrate: the escape migration stranded every case-twin's contents
d8e8a306 jit: the aarch64 guest exited to C on every return too
041303b3 tests: don't assume the host has no bridge in the AF_BRIDGE link-dump check
7cdf507b fs/sock: an RTM_GETLINK dump must ignore the request's family (ip -4/-6 addr)
2b67f846 tests: the retcache A/B rig, generalized to any jit_fuse bit and shipped
e35caf33 jit: the riscv64 engine exited to C on every guest return
52a53202 perf: why riscv64 ties aarch64 -- the engine is dispatch-bound at ~6.8 ns/dispatch
fa2f5520 tests: integration check across the three merged work streams
9b01be87 perf: amd64 incdec_reg re-measured on the A9 -- ~2%, and 669c4c49's rig claim was wrong
669c4c49 jit: amd64 register inc/dec ran a C helper that re-decoded the instruction
14a35b15 tests: exec_i386_fault_addr was missing its need_file guard
536e65c8 jit: an i386 image inherited a 64-bit ancestor's fault address high half
2abe9a1f perf: amd64's bridge cost measured properly -- 16.5%, not 41.6%, and why
d35dd5f0 perf: clean four-arch benchmark on the A9, and bmt's EAGAIN attributed
56eeb4a0 docs: design for a no-copy Metal sgemm accelerator (milestone 1)
46cf47e2 tests: vfork_exec_stale_jit printed a verdict the runner could not see
a7769162 jit: an exec out of a shared mm kept running the old program's translations
7be32913 tools: the devuan rootfs builder could not run on an Apple Silicon host
914a30ec syscalls: i386 pread/pwrite silently truncated the 64-bit offset to 32 bits
55bd5dc3 tools: mount-root.sh threw away the caller's quoting, silently
145d612c llm chat: the deferred switch dropped it when the reply beat the dialog
e2e3f7e2 llm chat: fix what an adversarial review of the sessions work found
55dcae5a llm chat: rebuild the chat and destination menus on open, guard nil storage
aa1ed968 llm chat: stream replies from https providers too
f900f470 llm chat: multiple chats and switchable destinations
711b48f3 jit: stop zeroing ~40KB of scratch on every guest syscall
0f26b254 jit: extend the live fusion switches to the arm64 and riscv64 guests
d2bae733 jit: fuse the riscv64 call (JAL with rd != 0) into one gadget
64ece77c perf: correct the record -- my local build was -O0, and one in-tree cost claim is false
97886877 jit: x86_64 host lost the flags of every locked ALU op to the CAS that followed
cb45397f perf: push/pop fusion is 2.9% on ARMv8.0 -- closing the question d7e44ecf left open
c0580d94 jit: make the i386 fusion switches live, via /proc/ish/i386_jit_fuse
d7e44ecf perf: what the push/pop fusion (7d9a27c3) actually measures, including what it does not
f258a7c1 fs: make devtmpfs a real filesystem instead of an unconditional no-op
7d9a27c3 jit: fuse i386 push/pop of a register into one gadget
6711eea3 tlb: record the measured negative result for padding the TLB entry to 32 bytes
1934733a perf: ARMv8.0 numbers for the last three commits, and a correction
5fe00a09 terminal: add scrollback search, driving hterm's already-bundled find engine
52d4bbff jit: fuse i386 LEA into one gadget, ~12% -- the worst work:dispatch in the engine
20c5c9dd riscv64: forward-port the lui/auipc constant fold from the riscv branch
70bc2043 jit: riscv64 was syncing 3KB of cpu_state on every block, 27.5% of its time
a8cc7f5f jit: fuse i386 mov reg<->[base+disp] into one gadget, 5.0% on ARMv8.0
eb83bdf0 jit: fuse i386 ALU reg,imm into one gadget, 3.7% on ARMv8.0
b9db05e4 jit: record the ARMv8.0 measurement for the addr fusion
1edd8e0f jit: fuse the address computation into the i386 memory-operand gadgets
2c241c7c jit: riscv64 DOES benefit from dmb (1.53x) -- the flat result was one workload
3682b62a jit: withdraw the "dispatch is diluted" explanation for riscv64
51e4599b jit: correct the riscv64 half of 7e3d8246 -- it is insensitive to dispatch
1cc914da tests: fakefs_type_race watchdog now detects stalls, not slow hardware
7e3d8246 jit: default the arm64/riscv64 guest dispatch to dmb, 1.73x on ARMv8.0
49750e57 build: a scheme Environment Variable does not reach a build script phase
1ed40039 uname: report the arm64 gadget dispatch mode, so its A/B is falsifiable
0309c172 jit: make arm64-guest gadget dispatch selectable, to A/B it on ARMv8.0
01bb7978 jit: warn against the ldar dispatch substitution at the call site
95140ab7 Revert "jit: use ldar for x86 gadget dispatch instead of ldr + dmb ishld"
861da1d1 jit: use ldar for x86 gadget dispatch instead of ldr + dmb ishld
0e561a0e jit: export poke on the x86_64 host so the ret chain path links
910b9887 jit: drop the sigprocmask syscall from every block compilation
e2ce804e jit: link i386 backward edges, the last guest engine that could not
0f53e5e7 docs: add the iSH-AOK 547 release summary
```
