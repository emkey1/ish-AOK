iSH-AOK 548

92 commits. Two cycles landed together: a performance pass that made every
guest engine faster, and a filesystem pass that found four different ways for
two files to be mistaken for one.

Files in different roots could be mistaken for the same file. fakefs numbers
its own inodes out of SQLite, but built the rest of the stat result by calling
through to the real filesystem, so the inode came from the database while the
device number came from whatever host volume the backing store happened to sit
on. Every fakefs mount on one volume reported the same device while numbering
its inodes independently from 1. Since the whole of userland treats a matching
device-and-inode pair as "this is the same file, skip the work", `cp` between
two roots refused with "are the same file" and copied nothing. The sharpest
case needed no coincidence at all: every fakefs database numbers its root
directory inode 1, so the root directories of any two roots were literally the
same file.

One file's metadata could be attached to another file's path. Inodes were
allocated from an in-memory counter seeded once from the database high-water
mark, so a second allocator on the same metadata -- another process on the same
root, or that root mounted twice -- handed out numbers the first was still
using. The resulting constraint violation was only logged, and the write that
followed it ran anyway. The damage was invisible until the two paths disagreed
about being a directory, at which point the name could not be removed by
anything: `rmdir` refused because the host entry was not a directory, `unlink`
refused because the metadata said it was. One root could no longer compile
anything, because gcc kept picking a temporary name that had been poisoned.

Every guest engine got faster. The aarch64 and riscv64 guests were exiting to C
on every single guest return and now have return caches; the i386 engine gained
fused gadgets for address computation, ALU, mov, LEA and push/pop; and a 40 KB
`memset` that ran on every guest syscall in every engine is gone. The engine is
dispatch-bound at about 6.8 ns per dispatch, which is precisely why fusing
instructions pays.

Statically linked i386 binaries died in libc startup when launched from an
aarch64 root, while the identical binary run in place worked fine. The fault
address is a 64-bit field shared by all guest ABIs, the i386 engine wrote it
with a 32-bit store, and nothing cleared the high half across an exec that
changes ABI. It looked filesystem-specific and was not.

Bugs the release testing caught: an empty cgroup could not be removed, so
systemd never reclaimed a single one; an `assert` in the Files provider turned
"you dragged in a folder" into a crashed extension; the Filesystems screen had
no way back out except deleting or exporting; `df` printed 100-character
app-group container paths instead of filesystem names; and the float80 test
suite, red on both hosts for months, turned out to be neither a printing bug
nor a float80 bug -- the compiler was folding the test's own reference values at
compile time, in the wrong rounding mode.

Validated: full guest suite on all four architectures, i386 115, amd64 116,
arm64 114, riscv64 107, zero failures, plus both arms of every architecture's
fusion switch. `meson test` green on the aarch64 and x86_64 hosts. On device,
an M4 iPad Pro ran the full suite on all four architectures and an A9 iPad ran
the curated set.

Known: three terminfo entries in one migrated root come back from the repair
with their symlink-ness inverted; the systemd user session on Arch cannot reach
its user bus; units requesting `PrivateNetwork=` cannot start, since network
namespaces are unimplemented; and a busy mount cannot be lazily detached.
