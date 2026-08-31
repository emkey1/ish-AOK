# Appendix A. Timeline

Upstream iSH and iSH-AOK on one axis. Dates are from the repository; the
commits named are the ones that mark a turn rather than the largest ones.

## 2017 — the first year

| | |
|---|---|
| **4 May** | Initial commit. `emu/memory.c` created the same day. |
| **8 May** | "Everything to get Hello World working" |
| **25 May** | **Ptrace-O-Matic** — differential testing against a real x86 kernel, in week three (Chapter 9) |
| 27 May | `brk`, `rep movs`, "really ghetto tls" |
| July | `xX_main_Xx()` — still the boot path today |
| **28 Oct** | **`fs/fake.c`** — bytes as host files, metadata in SQLite (Chapter 17) |
| 18 Oct | `app/Terminal.m` — hterm in a web view (Chapter 29) |
| December | Thread-safe fd refcounts; fds duplicated on fork |

**336 commits.**

## 2018 — threads, and the JIT

| | |
|---|---|
| **January** | Thread groups; `CLONE_THREAD`; fakefs schema redesigned for hardlinks |
| January–April | TestFlight builds 22–25 (`docs/CHANGELOG.md`) |
| **3 May** | "Foundations of jit, no actual compiling yet" |
| **26 May** | `gen()` written — unchanged since (Chapter 6) |
| 14 June | `jit/gen.c`, `jit/jit.c`, `jit/frame.h` |
| **17 Aug** | `jit/gadgets-aarch64/` and `jit_enter` |

**905 commits** — the project's largest year until 2026.

## 2019–2023 — consolidation, and other people

| | |
|---|---|
| **Sept 2019** | Xiangyan Sun's **return cache**, aarch64 then x86-64 (Chapter 6) |
| Jan 2019 | `fs/sockrestart.c` — surviving iOS taking listening sockets away (Chapter 19) |
| **Dec 2020** | **First iSH-AOK commits** — `/proc` entry ownership |
| Jun 2020 – Oct 2021 | **The GPLv2 relicensing**: 25 contributors, individually |
| **Dec 2021** | "Print the process name as well as the pid" — the fork's method, in one line (Chapter 3) |
| 2022 | The fork's first substantial year: lock instrumentation, leak fixes, upstream merges, Jason Conway's reordering patch (Chapter 6) |
| Late 2023 | The fork's reference-counting work, narrated in public while broken |
| **17 Nov 2023** | Upstream's last commit in this tree |

## 2026 — the year the tree tripled

| | |
|---|---|
| February | 28 commits — restart |
| March | 107 |
| **April** | **400 — the amd64 port**: the ABI split, and nine years of 32-bit assumptions removed |
| May | 59 |
| June | 373 — conformance, the crypto and pixman accelerators |
| **1 July** | **arm64 guest** — adapted from `OpenMinis/ish-arm64` with attribution |
| **10 July** | **riscv64 guest** — nine days later; "grep `GUEST_ABI_ARM64` and mirror every site" |
| July | 660 commits; HLE (Chapter 8) |
| **August** | **589 — native programs**: the shim, bash, zsh, OpenSSH, helix, Rust |
| 27 Aug | FUSE (Chapter 20) |
| 24 Aug | Shortcuts and App Intents (Chapter 32) |
| 31 Aug | Nested AOK proven, two levels deep (Chapter 42) |

**2,216 commits**, 27 releases (builds 521–551), roughly one every twelve days.

---

*Source:* `git log`. Commits-per-year: 2017 336, 2018 905, 2019 604, 2020 469,
2021 609, 2022 759, 2023 221, 2026 2,216.
