# 1. The impossible app

## 1.0 A demonstration

Here is an ordinary thing happening in an unusual place:

```
$ ./build/ish -f build/alpine-arm64-test /bin/sh
/ # uname -a
Linux Mac.lan 5.20.66-ish_aok iSH-AOK built 2026-08-31 10:24Z unoptimized aarch64 Linux
/ # head -4 /proc/cpuinfo
processor       : 0
model name      : iSH Virtual aarch64-compatible CPU @ 1.066GHz
BogoMIPS        : 1066.00
Features        : fp asimd cpuid aes pmull sha1 sha2 crc32 atomics sha3 sha512
/ # id
uid=0(root) gid=0(root)
/ # stat -c '%n %U:%G %a' /bin/busybox
/bin/busybox root:root 755
```

That is a genuine Alpine Linux userland — `busybox`, `apk`, `musl`, all of it
unmodified, all of it downloaded from Alpine's own mirrors — running as root on
a machine that has no Linux on it, no hypervisor, no container runtime, and no
second process.

The transcript above is the command-line build, running on a Mac, because a
Mac is easier to paste from than a phone. Change nothing but the embedder and
the same code is an iOS app: one App Store download, no jailbreak, no developer
mode, no sideloading, running on a device whose operating system will not let
an application do most of what a Linux userland assumes an application can do.

Every line of that output is manufactured. There is no Linux 5.20.66; the
version string is assembled in `kernel/uname.c`, and the word `unoptimized` in
it is a deliberate confession that this particular build was compiled at `-O0`
and its timings should not be believed. There is no processor 0: `/proc/cpuinfo`
is a string generated on demand, and the feature list in it is a promise about
which instructions the translator implements. `busybox` is owned by `root:root`
with mode 755 because a row in a SQLite database says so — the host file
holding those bytes is owned by whoever is running the app and is mode 644, and
nothing on the host has ever heard of uid 0. Even `MemFree` is a number
computed from Mach statistics about a process, reported as though it described
a machine.

This book is about how those lies are produced, why each one is necessary, and
what happens on the many occasions when a lie turns out to be insufficiently
detailed. That last case is the interesting one, and it is most of the work.

## 1.1 Five things an iOS application cannot do

Start with the constraints, because every major design decision in iSH-AOK is
downstream of one of them. None of these are bugs in iOS. They are the platform
working as intended, and they happen to forbid, one at a time, almost every
mechanism a Unix system is built out of.

**You get one process.** An App Store application cannot launch a second
process running code that arrived after the app was signed. There is no
supported path from "the user just ran `apk add gcc`" to "an operating-system
process is now executing `/usr/bin/gcc`". The host kernel will not load that
file; it is not signed, it is not Mach-O, and it is not even the host's
architecture.

So a guest process is not a host process. It is `struct task` — a structure in
`kernel/task.h` that contains the guest's registers (`struct cpu_state cpu`),
its memory map, its file descriptor table, its signal state, and a `pthread_t`.
`fork` allocates one of those and calls `pthread_create`. Everything that
follows from "a process is a thread" follows for the rest of the book: process
groups and thread groups have to be rebuilt by hand (Chapter 10); a signal is a
bitmask plus a `pthread_kill` to nudge the target out of a blocking host call
(Chapter 12); and, most sharply, a native program compiled into the app cannot
fork at all, because a C function running on a guest task's thread has no
address space to copy — which is why the native bash serializes its own state
into a script and re-launches itself (Chapter 24).

**You cannot write instructions and then execute them.** The entitlement that
permits mapping a page both writable and executable is not available to ordinary
App Store applications. This normally ends the conversation about JIT
compilation, and with it any hope of running an emulator at a tolerable speed.

iSH-AOK has a JIT anyway, because its JIT never writes an instruction. Compiling
a block of guest code means appending `unsigned long` values to an array —
`gen()` at [jit/gen.c:311](../../jit/gen.c#L311) is nine lines long and its
entire body is a bounds check, a `realloc`, and `state->block->code[state->size++] = thing`.
The values it appends are the addresses of small assembly routines that were
compiled, linked and code-signed at build time, interleaved with the immediates
those routines need. Running the block means loading the next pointer and
jumping to it. The array is data, so the platform has no objection to it, and
the technique — indirect threaded code, which Forth implementations were using
in the 1970s — turns out to be the thing that makes an emulator viable on a
platform designed to prevent emulators. Chapter 6 is about the consequences,
which are not all convenient.

**You are not root, and you never will be.** No `chown`. No `mknod`. No setuid
bits that mean anything. No mounting anything anywhere.

Every one of those is load-bearing in a Linux userland: package managers set
ownership, distributions ship device nodes, and `/etc/shadow` is mode 600 for a
reason. So the whole apparatus is faked. Chapter 17 covers fakefs in detail; the
one-sentence version is that file *contents* live in ordinary host files and
file *metadata* — uid, gid, mode, device numbers, the hardlink graph — lives in
a SQLite database beside them, and `stat()` is a query. The guest is root
because AOK reports that it is. That decision buys compatibility with every
distribution's install scripts, and it costs an entire class of test coverage:
when everything runs as uid 0, permission checks that are in the wrong order
never fail, and nobody notices until a guest finally creates an unprivileged
user (Chapter 15).

**The sandbox is the whole world.** The app gets a container directory. It
cannot see other applications' files, and other applications cannot see its.
The container lives on a filesystem with its own opinions — on APFS, one that is
usually case-insensitive, which is why installing Alpine's `ncurses-terminfo`
silently lost `/usr/share/terminfo/{a,e,l,...}` to their uppercase twins until
guest names started being escaped on the way to the host — `Foo` is stored as
`%foo`, and the rules in [fs/fake-path.h](../../fs/fake-path.h) also cover
Unicode case folding and NFC/NFD normalization, both of which APFS treats as
collisions and the guest does not.

There is also nothing that resembles `/dev`. Device nodes are entries in a
table (`fs/dev.c`), and a rootfs tarball that could not ship a real `/dev/null`
— Docker-exported images cannot, so Arch's does not — arrives with a plain
regular file sitting at that path, quietly accumulating every byte anything ever
writes to it. `setup_host_mounts()` in [main.c](../../main.c) repairs the
standard set at every boot, and reading that function is a good way to see the
shape of the problem: `/dev/null`, `/dev/zero`, `/dev/full`, `/dev/random`,
`/dev/tty`, `/dev/ptmx`, `/dev/fuse`, `/dev/kmsg`, `/dev/tty0`, `/dev/tty1`,
`/dev/console`, the `/dev/fd` symlinks that bash process substitution needs, a
`/dev/pts` to mount devpts on, and a `/dev/shm` with mode 1777 because
`wlroots` could not allocate a keymap without it. Each of those lines is a
distribution assuming something reasonable about a machine that does not exist.

**You can be suspended or killed at any moment.** iOS suspends applications
that are not in the foreground. A suspended emulator is a stopped world: from
the guest's point of view, time does not pass in any way it can detect, and a
`sleep 1` may take an hour of wall clock. Worse, the OS actively kills
applications that hold a file lock across suspension — the `0xdead10cc`
termination — and applications that exceed their memory budget, which is
jetsam.

Both of those reach into what look like purely kernel-internal decisions.
`host_mem_headroom_low()` in [platform/platform.h](../../platform/platform.h)
exists so that guest `mmap`, `brk` and `mremap` start returning `ENOMEM` while
there is still memory left, on the theory that a guest program handling
allocation failure is a better outcome than UIKit failing to allocate and taking
the entire app down with it. And a clean suspension requires driving fakefs to a
point where no SQLite transaction is open, which means a drain with a deadline
and a way for guest tasks to park rather than spin (Chapters 17 and 28).

> **The bug that taught us this**
>
> There is no membrane between the guest and its host. `cli_halt()` in
> [main.c](../../main.c) — the function that runs when guest init exits —
> carries a comment explaining why it deliberately does *not* call
> `fflush(NULL)`. Flushing every stream walks the host's stdio list and takes
> each stream's lock. A native program is handed real host `FILE`s, so a guest
> task killed inside stdio leaves a host stream lock held by a thread that no
> longer exists, and Darwin does not release a mutex when its owner dies. Two
> `ish` processes were found sitting at 0% CPU, five and twenty-three hours
> after their guests had exited, blocked in
> `_fwalk → sflush_locked → flockfile`. In a real kernel, a dead process cannot
> hold your libc's locks. Here it can, because it was never a process and the
> libc was always shared.

## 1.2 What people wanted anyway

The demand this project answers is not "a terminal on my phone". Terminals for
iOS are easy, as long as the terminal is talking to something else.

What people wanted was for the phone to *be* the machine: to install packages,
compile things, run scripts they wrote elsewhere, edit files, and ssh outward
rather than inward. And the specific requirement inside that list, the one that
determines everything, is the package manager.

A shell you can write. An editor you can write. A package manager means running
binaries you did not build, produced by a distribution that has never heard of
your platform, linked against a libc that issues Linux syscalls by number and
reads Linux's `/proc` to answer ordinary questions. `apk add gcc` pulls down a
compiler that will immediately try to locate its own `cc1` by searching `PATH`,
`fork` a child, `execve` it, wait on it, and interpret its exit status. The
moment that has to work, the project has stopped being *Linux-like* and signed
up to *be Linux*, in enough detail that software written by people who have
never heard of it cannot tell the difference.

That is a much larger promise than it sounds, and the rest of this book is
mostly a record of it being kept in increments.

## 1.3 Three ways to try, and the one that was taken

**Put the Linux somewhere else.** Ship an ssh or mosh client. Everything works
perfectly, because a real Linux machine is doing all of it. This requires
owning a real Linux machine and having a network, which means the product is
not "Linux on your phone" but "a window onto Linux you own elsewhere".

**Reimplement a Unix-ish userland natively.** Compile the tools against iOS's
own libc and ship them inside the app. This is fast, because it is native
ARM code with no translation anywhere. It also means that every tool a user
might want has to be ported, by you, in advance; that anything you did not
anticipate is simply unavailable; and that there is no `apk add`, because there
is nothing for a package to contain. You get a curated toolbox, not a system.

**Emulate the machine and reimplement the kernel.** Then any unmodified Linux
binary runs — including the package manager, which then installs the binaries
nobody thought about. The price is speed, and a very long tail of fidelity work,
because "any unmodified binary" includes the ones that use the syscall you have
not implemented and depend on the `/proc` file you have not written.

There is a fourth approach that is not available: run a real Linux kernel in a
virtual machine. Hardware virtualization is not offered to third-party iOS
applications. Emulation is not a choice made for ambition's sake; it is the only
mechanism the platform leaves standing.

Upstream iSH took the third road in May 2017, and spent six years on the tail.
iSH-AOK's whole 2026 can be read as a campaign against the price: the gadget
JIT and its four guest translators (Chapters 6 and 7), high-level emulation that
skips the interpreter entirely for functions it recognizes (Chapter 8), and — in
the most interesting reversal in the project — native programs, which
reintroduce the second approach *inside* the third, as a fast path over a
working emulator rather than as a replacement for one (Part V). The toolbox
comes back, but now it is optional, and `apk add` still works.

## 1.4 Three sentences that describe the whole system

Everything in this book can be hung on three claims. Each is meant literally.

### The kernel is a library

There is no privileged code and no boundary. `libish.a` is an ordinary static
archive; [main.c](../../main.c) links it into a command-line binary and
`app/main.m` links the same archive into a UIKit application.

Booting is a function call. `xX_main_Xx()` — the name is a joke from 2017 that
nobody has had the heart to remove — parses arguments, mounts a root filesystem,
calls `become_first_process()` to create pid 1, calls `do_execve()` on the
program it was asked to run, wires up stdio, and returns. Then `main` calls
`task_run_current()`, and pid 1 begins executing on the thread that called it.
Shutting down is a callback in the other direction: the kernel invokes
`halt_hook`, and the CLI's implementation converts the guest's wait status into
the host process's exit code with the same `128 + signo` convention a shell
uses.

The iOS app does the same thing with different callbacks, which is why the
distinction between "the kernel" and "the app" is a matter of which functions
call which. It also means the kernel can be linked into a test harness, into
`ptraceomatic`, and — the useful consequence for this book — that almost
everything in Parts II through IV can be exercised from a terminal on a laptop
with no phone involved.

### The CPU is a dispatch loop

`struct cpu_state` is a member of `struct task`, not a peripheral. Executing
guest code means finding or compiling the block at the current guest instruction
pointer, then running an array of gadget addresses, each of which does a small
amount of work and tail-calls the next.

The cost of that dispatch is about 6.8 nanoseconds, measured, and it is the same
6.8 nanoseconds whether the guest instruction is an `add` or a `div`. This is
the single most important performance fact about the system, and Chapter 38
draws out the consequences: total time tracks guest instruction *count*, so the
optimizations that matter are the ones that reduce how many guest instructions
get dispatched, not the ones that make each dispatch cheaper. It is also why an
arm64 guest on an arm64 host is not free, which surprises everyone, including
the people who implemented it (Chapter 7).

### The filesystem is a database

Contents in host files, metadata in SQL, and a schema that has to be able to
express things the host filesystem cannot: a file owned by uid 0 in a container
owned by a mobile user, a character device with major 5 and minor 1, three
directory entries that are the same inode. `stat()` is a query. `rename()` is a
transaction. And a lock held across app suspension is a termination, which is
why Chapter 17 spends as much time on quiescence as on schema.

A fourth sentence would be *a process is a thread*, which is Section 1.1 and
Chapter 10, and a fifth would be *a device is a table entry*, which is
Chapter 18. Three is enough to start.

## 1.5 What the guest believes

The recurring question in this book, asked at every level, is what the guest
believes and what is actually true. Here is the smallest complete example.

> **What the guest believes**
>
> A guest `top` opens `/proc/stat` to find out how the system's CPU time is
> being spent. On a real machine that file describes the machine.
>
> The obvious implementation is Mach's `HOST_CPU_LOAD_INFO`, which reports
> exactly that: total user, system, idle and nice ticks for the whole device.
> It is also wrong, and wrong in a way that looks right in testing. On a phone
> that is running other applications, the device is mostly idle even while iSH
> is pinning a core, so the guest's `top` reported an idle system while the
> guest was flat out — and every load-sensitive script in the guest drew the
> wrong conclusion.
>
> `get_total_cpu_usage()` in
> [platform/darwin.c](../../platform/darwin.c) instead reports *this process's*
> cumulative user and system time, and derives an "idle" figure from wall-clock
> uptime multiplied by the emulated CPU count — because Mach has no per-process
> notion of idleness, and the guest requires one to exist.
>
> No such CPU exists. The number is nonetheless the only true answer to the
> question the guest is actually asking, which is "how busy am I?".

That is the shape of nearly every decision in the system: not "what is the
correct value" but "what is the correct value *for the question the guest is
really asking*", and the two are different often enough that assuming otherwise
is the most reliable way to introduce a bug.

## 1.6 What it costs, stated up front

A book about a system should say early where the system is weak, so the reader
can calibrate.

**There are no namespaces.** No PID namespaces, no mount namespaces. There is
exactly one Linux underneath everything, so `/proc`, `/sys` and `/dev` show the
same true state no matter which root you booted or which chroot you are standing
in. Nothing container-shaped works. This is architectural, not a missing
feature; Chapter 21 shows the one place it is an advantage, and Chapter 41 is
honest about the rest.

**`PROT_EXEC` is never enforced.** Guest pages have no NX. A guest program that
jumps into its own data will run it. Chapter 13 explains why fixing that is a
project rather than a patch.

**Everything is one process, so everything is one blast radius.** An assertion
failure in a routine `/proc/meminfo` read aborts the whole application, terminal
and all, which is why `get_mem_usage()` degrades to best-effort values instead
of asserting when the Mach calls fail — they started failing on newer iOS
versions, and a guest running `free` would otherwise have killed the app.

**It is dispatch-bound.** See Chapter 38 for what that means in practice, and
for the measurements that say when it stops mattering.

**And the fidelity tail never ends.** There are more than 170 C
programs in `tests/manual/`, plus a handful of shell-script suites, and each one
exists because something behaved differently from Linux and somebody had to find
out why. Appendix F annotates them.

## 1.7 How this book is arranged

**Part I** is history: upstream iSH from 2017, the fork, and the year the tree
tripled in size.

**Part II** is the execution engine — the guest CPU model, the threaded-code
JIT, the four guest architectures, high-level emulation, and how any of it is
proven correct. It is the most self-contained part of the book and the one to
read first if you only read one.

**Part III** is the kernel: tasks, syscalls, signals, memory, futexes,
readiness, and program loading.

**Part IV** is filesystems — the VFS, fakefs, the synthetic filesystems,
sockets, FUSE, and the read-only filesystem that is compiled into the binary.

**Part V** is native programs, which is the fork's central idea and the part
with the least prior art: `execve` that dispatches to a host function, the libc
shim that makes such a function answer questions about the guest rather than
about the phone, the fork that cannot happen, and the licensing consequences of
compiling other people's software into your binary.

**Part VI** is the application: lifecycle, the terminal, roots, the Files
integration, the workspace, device integrations, and the optional accelerators.

**Part VII** is how it is built, tested, and shipped, including the debugging
techniques that work when there is no debugger and the system under test is
your own address space.

**Part VIII** is performance: where the time goes, how it was measured, and six
optimizations told end to end.

**Part IX** is what the project believes, what is honestly missing, and where it
could go next.

Chapters are anchored: each one ends with the files it was written from, and the
ones that exist because of a specific failure name the bug. Where a claim is a
measurement, the method is given with it. Where a claim is a guess, it says so.

> **Measure it yourself**
>
> Everything in Parts II through IV can be run from a laptop. Build the
> command-line emulator, convert a rootfs tarball into a fakefs, and boot it:
>
> ```sh
> meson setup build --buildtype=debugoptimized
> ninja -C build
> ./build/tools/fakefsify alpine-minirootfs-3.23.3-aarch64.tar.xz alpine
> ./build/ish -f alpine /bin/sh
> ```
>
> Use `--buildtype=debugoptimized`. Meson's default is `-O0`, and an `-O0`
> emulator does not merely run slowly, it invalidates every measurement taken on
> it — which is why such a build says `unoptimized` in `uname -v`, as the
> transcript at the top of this chapter does.

## 1.8 The two questions

The rest of this book asks the same pair of questions at descending levels of
the stack, and it is worth stating them plainly once before starting.

*What does the guest believe?* — which is usually easy, because the guest is
Linux software and Linux is well documented.

*What is actually true?* — which is usually a Mach call, a `pthread`, a row in
SQLite, or a number that had to be invented.

Every chapter is a place where the answers diverged, and something had to be
built to reconcile them. Where they were reconciled well, nobody ever notices.
Where they were reconciled badly, there is a test in `tests/manual/` named after
it.

---

*Anchors:* [main.c](../../main.c), [xX_main_Xx.h](../../xX_main_Xx.h),
[kernel/init.c](../../kernel/init.c), [kernel/task.h](../../kernel/task.h),
[platform/platform.h](../../platform/platform.h),
[platform/darwin.c](../../platform/darwin.c), [jit/gen.c](../../jit/gen.c),
[fs/dev.c](../../fs/dev.c), [fs/fake-path.h](../../fs/fake-path.h),
[README.md](../../README.md).

*Story:* the `fflush(NULL)` hang — two `ish` processes alive at 0% CPU five and
twenty-three hours after their guests exited, each blocked on a stdio lock whose
owning thread had been killed.
