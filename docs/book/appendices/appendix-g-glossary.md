# Appendix G. Glossary

Terms as this book and this codebase use them. Where a word means something
different here from its usual sense, that is said.

**aokfs** — the synthetic read-only filesystem mounted at `/AOK`, served from C
string tables compiled into the binary. Chapter 21.

**applet** — one command inside a multicall native program, selected by
`argv[0]`, exactly as busybox does. Chapter 25.

**basic block** — a run of guest instructions with one entry and one exit. The
unit of translation.

**blessed allocation** — a guest mapping contiguous enough in host memory to be
handed to a GPU or accelerator without copying; specifically a
`MAP_SHARED|MAP_ANONYMOUS` region that survives `fork` as one host region.
Chapter 33.

**chaining** — patching one translated block to jump directly to another instead
of returning to the C dispatch loop. Chapter 6.

**dispatch** — moving from one gadget to the next. About 6.8 ns, and the
quantity that determines almost all emulator performance. Chapters 6 and 38.

**fakefs** — the filesystem that stores contents as host files and metadata
(mode, uid, gid, rdev, hardlink structure) in SQLite. Chapter 17.

**fusion** — emitting one gadget for what would otherwise be two or more, decided
at translation time. Runtime-togglable per guest through
`/proc/ish/<arch>_jit_fuse`. Chapters 6 and 38.

**gadget** — a small assembly routine implementing one piece of guest semantics,
compiled and code-signed at build time. The only executable code in the JIT.
Chapter 6.

**guest** — the emulated Linux side. Contrast **host**, meaning iOS or macOS and
its libc.

**HLE (high-level emulation)** — recognizing a known guest libc function at
translation time and running a native implementation of its *contract* instead
of translating its instructions. Chapter 8.

**jetsam** — two unrelated things, unfortunately. In `jit/`, the list of
invalidated blocks awaiting a hand-built RCU grace period (Chapter 6). On iOS,
the memory-pressure killer whose budget `host_mem_headroom_low()` watches
(Chapter 13).

**may_block** — the filesystem flag meaning "operations on this filesystem can
block on userspace", which makes callers drop `inodes_lock` across them.
Chapter 20.

**native program** — host code compiled into the app and reached by `execve` of a
path under `/AOK/native`. Not a process: a C function on a guest task's thread.
Part V.

**oracle** — an independent implementation of the same contract, trusted more
than your own. Real x86 silicon, the Unicorn engine, a real Linux machine, or
the emulator in a different configuration. Chapter 9.

**paravirtual** — an accelerator the guest must deliberately ask for, through a
provider or a preloaded shim, rather than one the emulator applies invisibly.
Chapter 33.

**quiesce** — driving fakefs to a state where no transaction holds a lock, so the
app can be suspended without being killed. Chapters 17 and 28.

**re-launch** — a native program's substitute for `fork`: serialize its own state
into a script and start a fresh copy of itself to run it. Chapter 24.

**root** — an installed guest filesystem: a `data/` directory plus its
`meta.db`. Not the user `root`, which this book writes as uid 0 where ambiguous.
Chapter 30.

**shim** — the libc replacement force-included ahead of the system headers for
native programs, so their libc calls answer questions about the *guest*.
Chapter 23.

**tier0** — the differential test sweep that auto-discovers every
`tests/manual/*.c` including `test_common.h`. Passing tier0 is not evidence the
guest suite runs your test. Chapter 9.

**threaded code** — representing a compiled program as a list of addresses of
pre-compiled routines rather than as machine instructions. The technique that
makes a JIT possible without a JIT entitlement. Chapter 6.

**W^X** — "write xor execute": the policy that memory may be writable or
executable but not both, enforced for third-party iOS applications. Chapter 6.
