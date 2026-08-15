# bash as a native program: what it needs, and the one thing that stops it

Assessment written 2026-08-15, against bash 5.2.21 configured and built on this
Mac (arm64 Darwin), which is the same compiler and libc a native program in
iSH-AOK is built with. Every number below is measured from that build rather
than estimated.

The short version: **the libc surface is not the problem, the build system is
not the problem, and symbol collisions are not the problem. `fork()` is.** Six
of bash's seven fork sites need the child to go on running bash's own C code,
and that is the one thing a native program cannot do.

## Why bash rather than exsh

exsh embeds PSCAL's VM; bash does not, and bash is enormously more mature. AOK
is GPLv3, so bash's licence is compatible. Nothing in this document argues
against the choice — it argues about sequencing.

## 1. fork: the blocker

bash funnels every fork through one function, `make_child()` in jobs.c
(nojobs.c when job control is compiled out). That is architecturally the same
shape as SmallCLUE's `spawn.h` seam, and it is tempting to conclude the same
fix applies. It does not, because the two differ in what the child *does*.

SmallCLUE's children exec immediately: 7 of its 8 fork sites do at most a
`setpgid` before `exec*`, which is why expressing them as "spawn this argv"
worked and why `timeout`, `xargs`, `env` and `nohup` run natively today.

bash's do not. The seven `make_child` call sites:

| site | construct | child runs |
|---|---|---|
| execute_cmd.c:654 | `( ... )` subshell | bash code |
| execute_cmd.c:2421 | coprocess | bash code |
| execute_cmd.c:4107 | null command in a pipeline | bash code |
| execute_cmd.c:4443 | any simple command **in a pipeline or async** | bash code |
| execute_cmd.c:5652 | external command in `execute_disk_command` | exec |
| subst.c:6536 | process substitution `<(...)` | bash code |
| subst.c:7009 | command substitution `$(...)` | bash code |

Only one of the seven is spawn-shaped, and it is the one AOK can already do.

The 4443 case is the sharpest, because it is not an exotic construct. Its guard
is `pipe_in != NO_PIPE || pipe_out != NO_PIPE || async` — so `ls | wc -l` forks
twice, and each child then continues inside bash: word expansion, then a
decision about whether the word names a builtin, a function, or a disk command.
The exec, if it happens at all, is many function calls later. `x=$(date)`,
`cmd &`, `( cd /tmp && ... )` are all the same story.

Making those work means the child must be a second bash with the parent's
entire mutable state — variables, functions, options, traps, redirections, the
job table. Three ways out, none free:

1. **Serialise and re-launch.** Spawn a fresh `bash` task and hand it the
   state. bash already exports functions through the environment, so part of
   the machinery exists. Unexported and local variables do not cross, which is
   a real semantic break rather than a slow path: `x=1; ( echo $x )` would
   print nothing.
2. **vfork-shaped `fork()` in the shim.** `sigsetjmp` at the fork point, return
   0, let the "child" run on the caller's own thread, and have `exec*` snapshot
   the descriptors it was left with, spawn the real task, restore the caller's
   fds and `siglongjmp` back with the pid. This makes *every* fork+exec idiom
   work unmodified — including Nextvi's `cmd_make`, which is what needed
   patching for `!!`. It is also exactly as dangerous as vfork: anything the
   child does between fork and exec that is not descriptor, signal or
   process-group manipulation corrupts the parent, and bash's children do a
   great deal more than that before they exec. It would fix site 5 and break on
   the other six.
3. **Accept the subset.** Ship native bash as a fast non-interactive command
   runner with no subshells, no command substitution and no pipelines, and keep
   the emulated `/bin/bash` for everything else. This is honest but the
   resulting shell is not one anybody would want as their shell.

None of these is small, and the choice between them is a design decision rather
than an implementation detail. **This is the thing to decide before vendoring
anything.**

## 2. exec-in-place changes the pid

`nlibc_exec_common` implements exec as spawn-then-exit, so the pid changes.
`nohup`, `chroot` and `env` do not care. A shell does, in bounded but real
ways.

bash reaches exec-in-place in two places: the `exec` builtin, and the
`CMD_NO_FORK` optimisation where `bash -c 'prog'` execs `prog` directly rather
than forking. In both, bash is *replaced*, so bash itself never observes the
change — its parent does. What breaks is anything keyed to the old pid:

- If the shell was a **process-group or session leader**, it stops being one.
  The new task inherits the pgid and sid (spawn defaults to `PGID_INHERIT`), so
  the group and session still exist and `kill -TERM -$pgid` still works — but
  the leader pid now names a dead task, and the tty's session association is
  built on that pid (fs/tty.c).
- A parent's **job table entry** points at the old pid. `wait` on it still
  works, because the old task exits with the child's status propagated, but the
  process that is actually running is not the one recorded.
- `kill -HUP $$` from a script that captured `$$` before an `exec`.

This is survivable for `bash -c`, which is how the PSCAL harness uses it and
how most scripts do. It is not survivable for a login shell. Worth fixing
eventually by teaching exec-in-place to reuse the caller's pid — the task
already exists and is not running its old image any more, so this looks
tractable — but it is not on the critical path.

## 3. The libc surface: smaller than expected

`tools/check-native-libc.py` over bash's 154 objects (readline and intl
excluded), with readline's own symbols filtered out, gives 127 calls that would
need an answer. Of those:

- **62 are already routed** by `kernel/native_libc.h` and would simply work:
  open, close, read, write, fork(refused), pipe, dup2, fcntl, ioctl, select,
  waitpid, kill, tcgetattr, tcsetattr, tcgetpgrp, tcsetpgrp, getpgrp, setpgid,
  getpwnam, socket, connect, getaddrinfo, and the rest.
- **33 are pure** — wide-character and locale conversion (`mbstowcs`,
  `wcswidth`, `wctype`, ...), `strcoll`, `strtold`, `imaxdiv`, `abort`. These
  belong in the tool's `PURE` set, and are one commit.
- **32 are real kernel calls that need routing**: `alarm`, `execve`,
  `faccessat`, `fchmod`, `getdtablesize`, `getentropy`, `gethostname`,
  `getrlimit`, `setrlimit`, `getrusage`, `killpg`, `mkdtemp`, `mktemp`,
  `pathconf`, `confstr`, `pselect`, `setitimer`, `umask`, the `getgrent` and
  `getservent` families, `fpurge`, and the four `dl*`. Every one of these has a
  precedent in the shim or a syscall AOK already implements; the `dl*` four
  should refuse, since loadable builtins cannot work on iOS anyway.

That is on the order of a day's work, and it is well-understood work.

`sigaction` and `stat` appear in the unrouted list despite being redirected,
which means some bash call site is not matched by the function-like macro —
worth a look, not a problem.

## 4. The build: tractable, but not a glob

SmallCLUE was easy because it is a flat `src/*.c`. bash is a configure project.
What that actually costs:

- **~103,000 lines of C** to vendor (bash's own `*.c`, `builtins/`, `lib/sh/`,
  `lib/glob/`, `lib/tilde/`) — against SmallCLUE, which is comparable in scale.
- **`config.h`, 1231 lines and 279 defines.** Generated by configure, and here
  is the useful part: a native program *is* host code, so configure is
  describing Darwin, not the guest. A plain macOS configure produces a valid
  header. The answers that must then be overridden by hand are the ones about
  runtime behaviour, which should describe AOK rather than macOS, plus an iOS
  variant since configure cannot run its tests when cross-compiling. Checking
  in one `config.h` per platform is the standard answer and is what this should
  do.
- **Four generator programs** — `mkbuiltins` (turns `builtins/*.def` into C),
  `mksignames`, `mksyntax`, `psize.aux` — that are built for the *build*
  machine and run during the build. meson does this natively with
  `executable()` + `custom_target()`, and AOK already uses that pattern for
  `aok_generated_tests` and friends.
- **No bison needed**: the tarball ships `y.tab.c` and `y.tab.h` pre-generated.
- **Start with `--disable-readline`.** readline is another large body of code
  that does its own terminal handling, and the terminal is the part of the
  native seam with the least mileage on it. An interactive native bash wants it
  eventually; a first cut does not.

**Symbol collisions are almost nil.** bash defines 1837 global symbols,
`libish.a` defines 1775, and exactly two overlap:

- `main` — same as SmallCLUE, renamed with a `-D` at compile time.
- `wait_for` — bash's `jobs.c` against AOK's `util/sync.h`. One `-D`.

That is a far better result than the size of the two codebases suggests.

Note that Nextvi has just needed its own archive (meson.build) so a `-D` could
apply to one translation unit and not the rest. bash needs the same treatment
for its two renames, and `tools/check-native-libc.py`'s `DEFAULT_TARGETS` must
gain the new archive — the gate reads "clean" over whatever it is pointed at,
and pointing it at half the native code is the failure it exists to prevent.

## 5. `/AOK/native` only. Do not shadow `/bin/bash`.

`opt/AOK/tools/native-links.sh` documents why `/usr/local/bin` shadowing went
badly: it precedes `/usr/bin`, and SmallCLUE's applets are *smaller*
implementations. The incompatibilities are per-flag, so no audit of the sources
finds them — `grep` was present and worked, just not with `-q`, and that took
the PSCAL harness from 217 passing to 116.

bash is not a smaller implementation of bash, so that argument does not apply
directly. But a bash without working `fork` **is** a much smaller bash, and it
is smaller in exactly the constructs every script uses. Shadowing `/bin/bash`
with it would break the 217-test regression on the first command substitution.

So: `/AOK/native/bash`, off PATH, opted into explicitly, exactly as
`native-links.sh` does for the applets. Revisit only once section 1 is
answered — and then the question is not "shadow or not" but "is this the same
shell", which by then will have a real answer.

A useful check exists for this already: `tools/native-applet-audit.py` derives
the exclusion list from what the shim refuses. It is now down to 10 applets
from 34, and `bash` should be run through the same reasoning rather than
judged by eye.

## Recommended order

1. **Decide the fork question** (section 1). Everything else is wasted if the
   answer is "accept the subset" and the subset turns out to be unusable.
2. Route the 32 kernel calls and add the 33 pure ones. Independently useful:
   they are the calls any second native program will want, not bash-specific.
3. Fix exec-in-place to keep the caller's pid (section 2). Also independently
   useful, and it removes a whole class of surprise.
4. Only then vendor bash, with `--disable-readline`, a checked-in `config.h`,
   its own meson archive, and the two `-D` renames.
