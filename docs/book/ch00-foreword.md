# Foreword: on standing somewhere

This is a book about iSH-AOK, and iSH-AOK is a fork.

That word does a lot of quiet work. It can mean a project that took a name and
changed a logo, and it can mean a project that took a working system and built
another storey on top of it. This one is the second kind, and the distinction
matters enough to spend a few pages on before the technical chapters begin —
because a reader who comes to Chapter 6 and learns how the gadget JIT works
deserves to know that the gadget JIT was not invented here.

**Upstream iSH is [`ish-app/ish`](https://github.com/ish-app/ish), created by
Theodore Dubois — `tbodt` on GitHub — with a first commit on 4 May 2017.** Nearly
every mechanism this book describes as *how AOK works* began there, and a
surprising amount of it is still, line for line, the same code.

## What was already there

The founding sequence is visible in the repository's own history, and it is
worth reading in order because each step is a foundation something later stands
on.

**May 2017 — an x86 decoder and an emulated CPU.** The first week's commits go
from "Implement enough instructions to run up to the system call" to "Everything
to get Hello World working". `emu/memory.c`, still the guest address space
today, was created on the very first day.

**May 2017 — Ptrace-O-Matic.** Three weeks in, before the emulator could run
much of anything, came `tools/ptraceomatic.c`: run the same program under a real
x86 kernel and under the emulator simultaneously, single-step both, and compare
after every instruction. Chapter 9 is largely about this tool, and the decision
to build it that early is the single most consequential thing anyone did to this
project's correctness. It says that the emulator's claims would be checked
against reality rather than against confidence.

**October 2017 — fakefs.** `fs/fake.c` — bytes as host files, metadata in
SQLite — appears five months in. The schema was redesigned in January 2018 to
support hard links, which is the design Chapter 17 describes. Every guest file
that reports `root:root` today does so through machinery laid down then.

**December 2017 to February 2018 — the process model.** Thread-safe file
descriptor refcounts, then thread groups, then `CLONE_THREAD`. `struct task`,
one host thread per guest task, `current` as a thread-local: the design Part III
rests on entirely.

**May to August 2018 — the JIT.** "Foundations of jit, no actual compiling yet"
lands on 3 May 2018. `jit/gen.c` and `jit/jit.c` are created on 14 June; the
aarch64 gadget files and `jit_enter` on 17 August.

That last one deserves to be specific, because it is the clearest possible
statement of what inheritance means here. Chapter 6 quotes `gen()` in full and
calls it "the entire code generator". Here is where that function comes from:

```
$ git blame -L '/^static void gen(/,+1' jit/gen.c
38b7253d8e emu/gen.c (Theodore Dubois 2018-05-26) static void gen(struct gen_state *state, ...
```

The most important twelve lines in this book were written on 26 May 2018 and
have not needed to change since. The same is true of the `gret` dispatch macro's
declaration, of the block-cache structure, of the jetsam free list, and of the
idea — the actual insight, the thing that makes an emulator possible on a
platform that forbids JITs — that a compiled block can be an array of addresses
of pre-signed gadgets rather than instructions.

The list does not stop there. `fs/proc.c`, `fs/tty.c` and the terminal's line
discipline; `fs/sockrestart.c` and the whole reconstruction dance that survives
iOS taking your listening sockets away; `tools/fakefsify.c`; `app/Terminal.m`
and `app/TerminalView.m` and the decision to run hterm in a web view;
`xX_main_Xx()`, still the boot path, still carrying the comment thanking a
Discord server for its name. All upstream. All still here.

Counted crudely: of the 255 source files in `emu/`, `jit/`, `kernel/`, `fs/`,
`util/` and `platform/`, 149 were created upstream and 106 by this fork. 
But that count understates the debt rather than measuring it, because
the fork's files are mostly *additions* — additional guest architecture, native
programs, accelerators — while the inherited ones are the load-bearing ones. The
line counts point the other way for the same reason: `kernel/calls.c` is
overwhelmingly fork code today, because it grew from one syscall table to four.
The structure it grew within is not the fork's.

## The others

iSH was never one person's work, and several of the contributions this book
describes as part of the engine came from elsewhere.

**Xiangyan Sun** (`wishstudio`) wrote the **return cache**, in September 2019 —
initially for aarch64, then ported to x86-64, along with a round of aarch64
assembly optimization. Chapter 6 spends several pages on that cache and on the
hash function it depends on. It is not AOK's idea.

**Jason Conway** (`jason-conway`) contributed MMX and packed-shift instruction
work in 2022–2023, and a memory-ordering fix whose credit is written into the
hot path of every single guest instruction AOK executes. The line is in
`jit/gadgets-aarch64/gadgets.h`:

```asm
    dmb ishld /* Jason Conway's Re Ordering patch (upstream PR #1944 */
```

Chapter 6 measures what happens when you try to remove that barrier. It stays.

**Saagar Jha** (`saagarjha`) worked across the app and the kernel from 2018 to
2023: crash-log encoding, socket receive and send timeouts, the migration of the
build to xcconfigs that the fork's build still uses, and the hooking framework —
added in January 2023, in its commit message's words, "out of sheer
desperation", and since rewritten here, which is its own kind of compliment.

**Ryan Hileman** (`lunixbochs`) implemented futex timeouts, `fcntl(F_SETFL)`,
the random-number device API behind `/dev/random` and `/dev/urandom`,
`/dev/zero` and `/dev/full`, and `sysinfo` fields — several of which appear in
Chapters 14 and 18 without further comment, because they simply work.

**Matthew Merrill** (`MatthewMerrill`) implemented SSE2 instructions — `PADDQ`,
`PSLLQ`, `PSRLQ` and their variants — and the meson option for declaring SSE2
support.

**Viktor Oreshkin** (`stek29`) built the clipboard device and introduced
`fs/devices.h`, replacing magic device numbers with names; the device-number
table Chapter 18 describes is that work.

**nimelehin** implemented vector instructions with NEON, `get_cpu_count()`,
per-CPU usage accounting, `/proc/loadavg` and `/proc/<pid>/statm`, and task
sleep-state tracking — most of what makes `top` in a guest show something other
than zeroes.

**Zhuowei Zhang** (`zhuowei`) added x87 instructions that ffmpeg needed
(`fxam`, `fcomi`, `fpatan`), the `GS`-prefix-after-`LOCK` decoding case, and
hard-link handling in `fakefsify`.

**Charlie Melbye** built the theme system in the app, which is still how a user
picks a colour scheme today.

And a long tail beyond that — `0b101`, `kkk669`, `woachk`, `SEProblem`,
`cristeahub`, `Mnpn03`, `wallisch`, `thunderkeys`, `sdushantha`, `pancak3`,
`lorenzodla`, `krystophny`, `alexismarquis`, `NoahPeeters`, `ELChris414`,
`AngeloHYang`, and others whose commits are in the log with their names on them.

## The permission that makes this book possible

There is one more contribution, and it is not code.

Between June 2020 and October 2021, upstream ran a relicensing effort: iSH is
GPLv3, and contributions after a named commit are additionally licensed under
GPLv2, so that the project can link with GPLv2 work such as Linux and QEMU. That
required asking every past contributor to agree individually, and
[LICENSE.md](../../LICENSE.md) carries the roll — twenty-five people who
each wrote a commit saying yes.

Alongside it sits [LICENSE.IOS](../../LICENSE.IOS), which is the document this
fork most directly depends on. The GPL and Apple's App Store terms are in
tension; the iSH developers wrote down that they do not wish that tension to
prevent "the otherwise-compliant distribution of **derived apps**" and committed
not to pursue a violation arising solely from it.

Derived apps. That is this one. iSH-AOK ships because upstream's copyright
holders decided in advance to let forks ship, and said so in writing, and it is
worth being clear that this was a choice they did not have to make.

(Chapter 26 explains where that promise stops. It binds iSH's copyright holders
and nobody else — which is why compiling bash into the binary is a build option
with a licensing discussion attached, and not a default.)

## What this fork did, and how to tell the difference

None of the above diminishes the work in the rest of this book. iSH-AOK added
three more guest architectures and the ABI infrastructure to hold them; native
programs and the libc shim that makes them answer questions about the guest;
`/AOK`; FUSE; the accelerators; the File Provider and Workspace and Shortcuts
integrations; and several years' worth of conformance work of the kind Part III
catalogues. Upstream's last commit in this tree is from November 2023; roughly
2,200 commits landed here in 2026 alone.

But the honest way to read the technical chapters is with a question attached:
*was this mechanism inherited, or built?* The book's anchors make that
answerable, and the command is short:

```sh
git log --diff-filter=A --format='%ad %an' --date=short -- jit/gen.c
```

Run it on any file this book cites and the tree will tell you who put it there.
Where the answer is 2017 or 2018, you are reading about foundations somebody
else laid, which a later project was fortunate enough to be able to build on.

For upstream iSH itself — the project, the app, the issue tracker, the community
— go to [`ish-app/ish`](https://github.com/ish-app/ish). This book is about what
happened next.

---

*Anchors:* `git log --reverse`, [LICENSE.md](../../LICENSE.md),
[LICENSE.IOS](../../LICENSE.IOS), [docs/CHANGELOG.md](../../docs/CHANGELOG.md)
(upstream's TestFlight-era build notes, 2017–2018),
[docs/CREDITS-aarch64.md](../../docs/CREDITS-aarch64.md) (the fork's separate
debt, to `OpenMinis/ish-arm64`), [jit/gen.c](../../jit/gen.c),
[jit/gadgets-aarch64/gadgets.h](../../jit/gadgets-aarch64/gadgets.h),
[tools/ptraceomatic.c](../../tools/ptraceomatic.c), [fs/fake.c](../../fs/fake.c),
[fs/sockrestart.c](../../fs/sockrestart.c), [xX_main_Xx.h](../../xX_main_Xx.h).
