# Appendix B. Repository map

Where to start reading, by directory. Line counts are tracked source
(`.c`, `.h`, `.m`, `.swift`, `.S`, `.py`, `.sh`).

| directory | files | lines | what it is | chapters |
|---|--:|--:|---|---|
| `emu/` | 27 | 29,289 | guest CPU state, memory, TLB, FPU, vectors | 5 |
| `jit/` | 48 | 36,670 | the gadget JIT, per-guest translators, HLE | 6, 7, 8 |
| `kernel/` | 90 | 56,383 | syscalls, tasks, signals, exec, native programs | 10–15, 22–27 |
| `fs/` | 71 | 37,035 | VFS, fakefs, procfs, sockets, FUSE, aokfs | 16–21 |
| `util/` | 15 | 2,245 | locks, lists, timers, lock statistics | 16, 36 |
| `platform/` | 4 | 606 | the host abstraction — Darwin, Linux, standalone | 1, 13 |
| `vdso/` | 6 | 76 | the VDSO the guest is given | 15 |
| `app/` | 286 | 54,131 | the iOS app: terminal, Workspace, roots, integrations | 28–33 |
| `tools/` | 45 | 7,172 | developer tools and build-time generators | 9, 23, 34 |
| `tests/` | 387 | 65,451 | the regression suite, e2e, per-arch assembly, the conductor | 9, 35 |
| `opt/AOK/` | 44 | 7,771 | what `/AOK` serves: docs, tools, ktop, provisioning | 21, 31 |
| `deps/` | 314 | 83,866 | submodules: hterm, smallclue, bash, zsh, helix, tokio | 25, 26 |
| `docs/` | 52 | — | plans, postmortems, benchmarks, release notes, this book | 41, 37 |

## If you are reading for the first time

**To understand the engine**, in order: `jit/gen.c`'s `gen()` (Chapter 6),
`jit/gadgets-aarch64/gadgets.h`'s `gret` macro, `jit/guest-arm64/logical.S` for
what a gadget looks like, then `jit/jit.c`'s `cpu_step_to_interrupt`.

**To understand the kernel**, read `kernel/task.h` top to bottom. A large
fraction of its fields carry a comment naming the bug that required them, and
it is the best single file in the tree for learning how this system fails.

**To understand the filesystem**, read `fs/path.c`'s `path_normalize`
(Chapter 16), then `fs/fake-conn.c`'s header comment (Chapter 17).

**To understand the fork's central idea**, read `kernel/native.h` — the whole
execution model is in its header comment (Chapter 22).

**To find out what is known-broken**, read `docs/TODO.md`. It is a lab
notebook, not a task list.

## The four registration points that fail silently

Each of these decides whether work is visible, and none of them errors:

1. `fs/aok-*.manifest` — a file not listed is absent from `/AOK` on device
   (Chapter 21).
2. `tests/manual/setup-regressions.sh` — two lines, `need_file` and
   `all_tests`; plus the manifest above (Chapter 9).
3. `kernel/native.c` — the registry; `/AOK/native` is served from it
   (Chapter 22).
4. The Xcode knobs mirroring meson's options — a feature with no knob never
   reaches the shipping build (Chapter 34).

---

*Source:* `git ls-files`, counted 2026-08-31.
