# 23. The shim: answering questions about the guest

A native program is compiled into iSH-AOK as host code, which means its libc
calls bind to Darwin's libc, which means this:

```c
int fd = open("/etc/passwd", O_RDONLY);
```

succeeds, and returns the wrong file.

Not an error. Not a crash. A valid descriptor onto the iPhone's own
`/etc/passwd`, which exists, which is readable, and which describes an operating
system the guest has never heard of. Chapter 22 called this the worst available
failure mode, and the header agrees: *silent wrongness*.

This chapter is about the layer that fixes it — roughly nine thousand lines of
`kernel/native_libc.c` — and, more interestingly, about the tool that proves it
was fixed, because getting this right by inspection is not possible and nobody
tried.

## 23.1 The governing question

The instinct is to sort libc into "pure" and "impure" and route the impure half.
That instinct is wrong, and the way it fails is instructive: it catches `open`
and misses `getuid`.

The question that actually works is written in the project's notes as:

> not "is this function pure?" but **"can this function's answer differ between
> the host and the guest?"**

`getuid` is pure in every sense a compiler cares about. It touches no
descriptor, allocates nothing, has no side effects. And it answers "who am I?"
about the wrong machine — the iOS account, which is called `mobile`, and which
is not root. `uname` is the same: no state, no I/O, and it reports Darwin.
`getenv`, `isatty`, `ttyname`, `gethostname`, `getpwnam`, `getloadavg`,
`time`, `localtime` — every one of them is harmless by the usual test and wrong
by this one.

## 23.2 Force-include, and the order that matters

The mechanism is a header, `kernel/native_libc.h`, force-included into every
translation unit of every native program by the build.

It works by `#define`, and the interesting engineering is in the ordering:

> The system headers are included FIRST, deliberately: the macros below must not
> be in effect while the real declarations are parsed, or the prototypes
> themselves get renamed. Once these headers are in, SmallCLUE's own
> `#include <fcntl.h>` and friends are no-ops, so the macros only ever rewrite
> call sites.

Include the system headers, *then* define the macros. Every later `#include` of
those headers is swallowed by its own include guard, so the redirection only
ever touches calls — never declarations.

One file must not be compiled this way, and says so at the top:

```c
// This file must NOT be compiled with the shim force-included -- it is the
// thing the shim redirects *to*, and needs the real libc.
#define NATIVE_LIBC_NO_REDIRECT
```

> **The bug that taught us this**
>
> Include guards are a global namespace nobody thinks of as one.
>
> bash brings its own globbing, in `lib/glob/glob.h`, and that header's include
> guard is `_GLOB_H_` — which is the same guard macOS's `<glob.h>` uses. So the
> shim including the system header silently swallowed bash's own, leaving
> `glob_filename`, `glob_pattern_p` and the `GX_*` flags undeclared in three
> files, "with no hint that a header had been skipped".
>
> The fix is `NATIVE_LIBC_OWN_GLOB`: a program that brings its own globbing
> builds with that defined and the shim stands aside. Standing aside is safe
> rather than a hole, because a program in that position is calling its own
> code, which is itself compiled with the shim.

## 23.3 Why the easy version is not available

There is a simpler way to do all of this, and another project does it: rewrite
the path. Turn `/etc/passwd` into
`/var/mobile/Containers/.../rootfs/etc/passwd` and then call plain libc.

That works when the guest filesystem is a real directory tree in an app sandbox.
AOK's is not. It is a fakefs (Chapter 17) — escaped host names plus a SQLite
database holding every mode and owner — so **there is no real path for libc to
open**. `open("/etc/passwd")` cannot be rewritten into any host path that would
give a native program the guest's file with the guest's permissions.

So the calls have to go through the VFS itself, which is why the shim is a
reimplementation of libc's I/O layer over `kernel/native_io.h` rather than a
path-mangling wrapper. The design constraint traces directly back to the
decision in Chapter 17 to keep metadata in a database.

## 23.4 Errnos, and choosing your failure mode

AOK's internal error values are negative Linux errnos. The host's are positive
Darwin errnos. The numbering is not the same.

Flipping the sign was the first implementation, and it is wrong the moment the
two disagree — with a symptom that is genuinely funny once it is understood:

> SmallCLUE formats its messages with the HOST's `strerror`, so Linux `ENOSYS`
> (38) came out as macOS `ENOTSOCK` — which is exactly the
> `df: /: Socket operation on non-socket` that surfaced this. `ENOENT` and a
> handful of others happen to share a number, which is why most errors looked
> fine and only the unusual ones were nonsense.

So there is a translation table. The part worth copying is its default:

> Anything not listed maps to `EINVAL` rather than being passed through, so a
> new guest errno produces a wrong-but-plausible message instead of a wildly
> misleading one from an unrelated part of the host's table.

An unmapped error is going to be wrong either way. The choice is between wrong
and *plausible* — a caller sees "invalid argument" and investigates the call —
or wrong and *actively misleading*, which sends them to look at sockets when
they should be looking at a missing syscall.

## 23.5 The gate, and the day the polarity flipped

`tools/check-native-libc.py` reads the built objects, extracts every host libc
symbol they reference, and compares that against a list.

Which list, and in which direction, is the whole story. From its docstring:

> This began as a denylist of calls to forbid, and that was wrong in a way that
> kept repeating. It was written for filesystem calls; then it had to grow for
> process control, then host-global state, then system identity when `uname`
> reported Darwin, then user identity when `whoami` answered "mobile" — the iOS
> account. **Every one of those was found by a person running the thing, never
> by this tool, because a denylist only knows the categories someone already
> thought of.**

That paragraph is the most useful thing in this chapter. A denylist encodes the
failures you have already had. It cannot encode the ones you have not, and in a
domain where the failure mode is *silent success*, the ones you have not had are
the ones that matter.

So the polarity is inverted. Anything referenced that is not on the **allowlist**
fails. And the allowlist is deliberately narrow — arithmetic, memory, strings,
formatting, sorting; things with no kernel state at all — so that adding to it
"is a deliberate act asserting *this genuinely cannot observe the host*".

The consequence is stated as a design goal rather than a side effect:

> The failure mode is the point: a missed call becomes a build error naming the
> symbol, instead of a guest quietly reading the device.

The same tool has a second mode that turns it from a gate into a plan.
`--report` classifies rather than failing: *already routed*, *pure*, *needs
work*. Point it at a new program's objects and the third list is the entire
porting job, enumerated, before a line of glue is written. Chapter 25's
catalogue was largely built by running that list down.

Two operational caveats, both of which have cost time:

**It is run deliberately, not wired into the build.** That is a choice — the
scan is slow and the allowlist changes rarely — but it means nothing forces it,
and a change can land without it having been run.

**It grades the built archives, and `ninja ish` does not refresh them.** Point
it at `build/lib*.a` after an incremental build of just the binary and it
faithfully reports on yesterday's objects. A green result from a stale archive
is worse than no result.

## 23.6 Where `#define` stops

A macro reaches only translation units AOK compiles. That is most of the shim's
consumers and none of the interesting ones.

An object from another toolchain — Rust, Go, a vendored library with its own
build system — imports the *real* symbol names, and no header anywhere can
change that. The failure is silent, and it is worst precisely where a path
exists on both sides:

> **The bug that taught us this**
>
> zlib's `gzopen("/tmp/x.gz")` inside a native program compressed the **host's**
> `/tmp`, and looked perfect. The file was created. The data was written. The
> return codes were correct. It was simply on the wrong machine.

For that case there is a second mechanism: rewrite the symbols in the archive
before it is linked, with `llvm-objcopy --redefine-sym`. Two decisions in
`tools/gen-nlibc-renames.py` are worth taking away.

**The rename list is generated from the header, not maintained beside it.**
"That rewrite has to name exactly the same set the header redirects, and a
hand-kept second list would drift the first time someone adds a route. So it is
read from the header, which is the one place that decides."

**`--redefine-sym`, not the linker's `-alias`.** An alias is global: it would
also redirect AOK's *own* calls, and the kernel must be able to reach the real
host `open()`. Rewriting symbols inside one archive is the only scoped version
of this operation.

There is also a small SKIP set, each entry carrying its reason. `__tls_get_addr`
is one: Rust's standard library reaches it through its own thread-local
machinery *before* the shim's per-task state exists, so routing it would run
shim code on a thread that has no current task.

## 23.7 Whether a library can be linked at all

The practical test, learned from doing it:

**Split the library's API by whether a call touches a descriptor, a path, a
clock or a device.** If it splits cleanly, the pure half links as-is and only
the impure half needs reimplementing over the redirected calls. zlib split
perfectly: `deflate` and `inflate` are pure transforms over caller-owned
buffers, and only the `gz*` file layer had to be rewritten.

If it does not split — if I/O runs throughout, as it does in an editor, a
language runtime, a browser engine — then a header cannot help, and the only
route is link-time symbol rewriting over the whole thing. That is expensive and
it is not hypothetical: it is exactly the route Rust took, and Chapter 25 tells
that story.

## 23.8 Everything that is not a file

Files are the obvious half. The rest of the shim exists because of the question
in Section 23.1, and the list is longer than anyone expects:

**Identity.** `getpwnam`, `getpwuid` and friends read the *guest's* passwd
database, because the host's describes iOS. Rust's `home_dir()` cannot be
routed at all — it has its own logic — so the shim asks the passwd database
directly instead.

**Environment.** A native program's environment is `task->native_env`, seeded
from the `execve` that started it — kept on the task rather than in a global
"because two native programs really can run at once, one per guest task".

**Terminal capabilities.** `kernel/native_termcap.c` answers from the guest's
terminfo, not the host's. A program that describes the terminal from the host's
database drives it with the wrong escape sequences.

**Locale.** Subtle enough to have been wrong twice: iOS spells the UTF-8 locale
`"UTF-8"` and has no other. But `"UTF-8"` is a *charset*, not a locale — so it
belongs in `LC_CTYPE` and must not be handed to `LC_ALL`, and the locale a
program actually gets comes from the guest's environment.

**The kernel log.** `dmesg` works, because `klogctl` is routed to AOK's own log
— "AOK's guest is the Linux `klogctl` was asking about".

**Name resolution, time, load average, `/etc/hosts`, `/etc/resolv.conf`, rc-file
locations.** All of them from the guest.

> **The bug that taught us this**
>
> The shim's own state has to be per-task, and this was learned three times in
> one day, each from a separate user report:
>
> - `nlibc_std[3]` — the shim's stdio streams — was process-global, so an
>   all-native pipeline like `cat x | less` produced nothing at all.
> - `nlibc_pw` and `nlibc_pw_line`, the passwd lookup buffers, were shared: two
>   concurrent native `ssh` runs **segfaulted the whole app 8 times in 20**, on
>   a `strlen(NULL)` inside `pwcopy`.
> - `optind`, `optarg`, `opterr` and `optopt` resolved to libSystem's — one
>   process-wide copy shared by `ssh`, `ssh-keygen`, `sftp`, `scp` *and*
>   SmallCLUE's core — so a concurrent `ssh -G` lost its own flag to another
>   program's argument parsing.
>
> All three are the rule from Chapter 22 arriving in the shim itself: the
> process outlives the program, and here it also runs several of them at once.
> The fix in each case was `__thread`, and the lesson recorded alongside is that
> they "were found one report at a time rather than by sweeping".

## 23.9 Signals cannot be handled the ordinary way

One routing problem has no clean answer, and the shape of the compromise is
worth understanding.

A native program cannot give the kernel a signal handler. Installing one would
mean the guest CPU jumping into host code at an arbitrary point, which is not a
thing the emulator can do.

So the shim blocks the signal on the program's behalf and runs the handler at
the next `native_checkpoint` (Chapter 22). That creates a contradiction, spelled
out on the field that resolves it:

> Blocked means "do not wake this task" everywhere else in the kernel, which is
> exactly wrong here: the task must wake, so that its next checkpoint can run
> the handler.

Hence two sets on the task: `native_prog_blocked`, for signals the shim is
blocking on the program's behalf, and `native_held`, for what the program has
blocked for itself — "kept apart, because that half must go on meaning what it
says".

The symptom when this was missing is the kind that gets reported as flakiness:
`^C` during `sleep 30` under a native bash did nothing until the *next*
keystroke — and the interrupted read then ate that keystroke too.

## 23.10 What the shim is

It is a second libc, written specifically to answer every question about the
guest rather than about the phone, force-included ahead of the real one, backed
by a symbol-rewriting pass for code AOK does not compile, and policed by a tool
whose allowlist is deliberately too small.

It is also unfinished, and visibly so: `--report` prints what remains, and the
project's own notes list the outstanding symbols by name. That is the right
state for it to be in. A shim that claimed completeness would be making exactly
the kind of assertion this chapter has spent ten sections showing cannot be made
by inspection.

---

*Anchors:* [kernel/native_libc.h](../../kernel/native_libc.h),
[kernel/native_libc.c](../../kernel/native_libc.c),
[kernel/native_io.h](../../kernel/native_io.h),
[kernel/native_io.c](../../kernel/native_io.c),
[kernel/native_syscall.c](../../kernel/native_syscall.c),
[kernel/native_termcap.c](../../kernel/native_termcap.c),
[tools/check-native-libc.py](../../tools/check-native-libc.py),
[tools/gen-nlibc-renames.py](../../tools/gen-nlibc-renames.py),
[kernel/task.h](../../kernel/task.h) (`native_env`, `native_prog_blocked`,
`native_held`), [meson.build](../../meson.build) (the force-include).

*Story:* `df: /: Socket operation on non-socket` — a Linux `ENOSYS` whose sign
had been flipped rather than translated, formatted by the host's `strerror` into
an error from an unrelated part of the table.
