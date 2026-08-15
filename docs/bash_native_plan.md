# bash as a native program: what it needs, and the one thing that stops it

Assessment written 2026-08-15, against bash 5.2.21 configured and built on this
Mac (arm64 Darwin), which is the same compiler and libc a native program in
iSH-AOK is built with. Every number below is measured from that build rather
than estimated.

The short version: **the libc surface is not the problem, the build system is
not the problem, symbol collisions are not the problem, and `fork()` is a
problem that measurement makes much smaller than it looks.** Six of bash's
seven fork sites need the child to go on running bash's own C code, which a
native program cannot do — but the replacement for that is affordable, for a
reason that is not obvious until you time it. See section 0.

## 0. The measurements that decide it

Taken 2026-08-15 on this Mac: `pscal-devuan-arm64` under `./build/ish` for the
emulated column, and the same bash 5.2.21 binary run directly for the host
column. Host bash is the ceiling a native bash could reach, since native code
*is* host code.

| | emulated in AOK | host (native ceiling) | ratio |
|---|---|---|---|
| arithmetic loop, 20k iterations | 4.17s cpu | 0.09s cpu | **46x** |
| string/expansion loop, 4k iterations | 29.3s cpu | 0.78s cpu | **38x** |
| one `( : )` subshell | ~2.5 ms | ~0.7 ms | 3.5x |
| one bash startup | ~15 ms | ~3.6 ms | 4x |
| one **native** task spawn (`smallclue true`) | **~1.6 ms** | — | — |
| one emulated ELF exec (`/bin/true`) | ~7.4 ms | — | — |

Two things fall out, and they point the same way.

**Interpretation is what emulation costs you: 38–46x.** That is bash's parser,
word expansion and arithmetic being translated instruction by instruction, and
it is precisely what compiling bash natively removes.

**Forking is not.** A guest `fork` costs ~2.5 ms because AOK's `sys_clone` is
native C in the emulator — it was never being emulated. A native task spawn is
~1.6 ms. So replacing bash's `fork` with "spawn a fresh native bash and give it
the state" costs about what bash already pays today, and possibly less.

That inverts the conclusion this document originally reached. The re-launch
design was dismissed as the expensive option; it is the affordable one, because
the expensive thing was never fork.

The honest qualifier: 38–46x applies to time spent *inside* bash. External
commands stay emulated unless they are native applets, so an end-to-end figure
for a real script is lower and depends entirely on the mix. Measuring that mix
on a real workload — the PSCAL suite is the obvious candidate — is what should
size the expected win before the work is finished, not before it is started.

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
job table.

### Why fork cannot simply be emulated

Not for want of cleverness: `fork` needs two threads of execution to see
*different* memory at the *same* addresses, and that is exactly what a process
boundary provides and a thread boundary does not. A native program shares one
address space with the whole app. The only ways around it are to make every
piece of bash's state reachable through one relocatable context pointer — a
rewrite, not a patch, against bash's thousands of globals — or to not share the
address space, which means a second task.

So: a second task. The question is what it is given.

### The design: re-launch, and why it is semantically right

Spawn a fresh **native** bash task, hand it the parent's state, and have it run
the command. Two things make this work far better than it first appears.

**A subshell is one-way by definition.** Everything a subshell does to its own
state is *meant* to be discarded — that is what makes it a subshell. So
"serialise state in, exit status and stdout out" is not an approximation of
fork's behaviour, it is the actual contract. Only the inbound direction has to
be complete, and it is finite: variables (including unexported and local ones),
functions, shell options, traps, positional parameters, `$?`, cwd, umask. Open
descriptors and redirections cross by inheritance already. bash can already
serialise every one of these — `declare -p`, `declare -f`, `set -o`,
`shopt -p`, `trap -p` — and `make_child` is *already handed* the subshell's
source text, because it needs it for the job table.

(An earlier draft of this document called the loss of unexported variables "a
real semantic break". That was wrong: it is a break only if you transfer
nothing but the environment, and there is no reason to stop there.)

**It costs what fork already costs.** Section 0: ~1.6 ms for a native task
spawn against ~2.5 ms for the guest fork bash performs today. Serialising a few
dozen variables is native work on top of that, well under a millisecond.

A refinement that removes most of the traffic: the fork at
`execute_cmd.c:4443` fires for any simple command in a pipeline, and the great
majority of those end in an exec. Those can keep using the plain spawn path
that already exists (`native_spawn_opts`), leaving re-launch for genuine
subshells, command substitution and process substitution.

### What is genuinely hard

- `$!` and the job table across a re-launch, so `wait` and `jobs` stay honest.
- `BASH_SUBSHELL`, `SHLVL`, `BASHPID` — each has a defined value in a subshell
  and must be set rather than inherited.
- `$RANDOM` continuity, which is observable and is seeded state.
- Coprocesses and process substitution are longer-lived than a subshell but
  still one-way at creation, so they follow the same shape with more plumbing.
- Buffered stdio at fork time. A real fork duplicates unflushed buffers, which
  is a POSIX wart scripts occasionally trip over; re-launch would not. Arguably
  an improvement, but it is a behaviour difference and belongs in a list of
  them.

### The alternative that was considered and rejected

A vfork-shaped `fork()` in the shim — `sigsetjmp` at the fork point, return 0,
let the "child" run on the caller's own thread, and have `exec*` snapshot the
descriptors it was left with, spawn the real task and `siglongjmp` back with
the pid. It makes every fork-then-exec idiom work unmodified, which is genuinely
attractive; it is what Nextvi's `cmd_make` needed and got a spawn seam for
instead. But it is exactly as dangerous as vfork: anything the child does
between fork and exec beyond descriptor, signal and process-group manipulation
corrupts the parent, and bash's children do a great deal more than that. It
fixes site 5 — the one site that already works — and breaks on the other six.

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

## The fork work, as it now stands (2026-08-15, after building it)

Native bash is in the app and runs, and `fork` turned out to matter more
immediately than this document implied. `/etc/profile` line 4 is

    if [ "$(id -u)" -eq 0 ]; then

-- command substitution on the first executable line of the first file any
login shell reads. And bash does not treat a failed fork as a warning:
jobs.c's make_child does `sys_error("fork")` then `throw_to_top_level()`, which
ends the shell. So the observable behaviour is an instant exit, and native bash
is not usable as a shell until this is done. It is fine for `bash -c` of
anything that does not fork.

### Two tiers, because one of them cannot be general

**Tier 1 -- general, and not bash's problem.** Implement `posix_spawn` and
`posix_spawnp` in the shim over `native_spawn_opts`, which already does the
file-actions work. Any program using them then needs NO patch, which retires
the per-program spawn seams -- SmallCLUE has one, Nextvi needed one added
upstream today, and that is the per-instance pattern this project already
learned to reject once with check-native-libc.py.

**Tier 2 -- re-launch, and necessarily bash-specific**, because only bash can
serialize bash's state.

### Explicitly NOT the vfork trick

`sigsetjmp` at the fork point, run the "child" on the caller's own thread, and
have exec snapshot the descriptors and spawn for real. It would make raw
fork-then-exec work with no patches anywhere. It is rejected because its
failure mode when a program forks and does NOT exec is silent corruption of the
parent -- and a loud refusal has been the right answer every other time this
codebase has faced that choice. `nlibc_fork` returning ENOSYS is ugly and never
wrong.

### The serializer, confirmed against the source

bash already has every primitive, and each returns a STRING rather than
printing, so the state can be built into a buffer with bash's own quoting
rather than a second implementation of it:

| what | primitive |
|---|---|
| every variable, exported or not | `all_shell_variables()` (variables.c) |
| every function | `all_shell_functions()` |
| scalar value, quoted to read back | `sh_single_quote()`, `ansic_quote()` |
| indexed array | `array_to_assign()` (array.c) |
| associative array | `assoc_to_assign()` (assoc.c) |
| function definition | `named_function_string()` (print_cmd.c) |

`print_assignment()` in variables.c is the model for which of these applies to
a given SHELL_VAR; it writes to stdout, which is why this is a buffer-building
sibling of it rather than a reuse of it.

Transfer is over a dedicated pipe rather than the environment -- no size limit,
and nothing leaks into the child's actual environment. The child learns to read
it from one variable naming the descriptor, checked in bash's startup.

### Order

Command substitution first: it is the most common, the most clearly one-way,
and the thing that unblocks `/etc/profile`. Then subshells, then pipelines,
then coprocesses and process substitution.

The fast path for `$(single external command)` -- spawn it directly with stdout
on a pipe, no bash re-launch -- is worth having AFTERWARDS as an optimisation
(it saves a shell startup), but it is not a stepping stone toward this and was
wrongly described as one: it builds on `native_spawn_opts`, which already
exists, and shares no machinery with the re-launch.

## Where the source lives

A submodule at `deps/bash`, pinning a repo of our own — the same arrangement as
`deps/smallclue` and its nested `nextvi`, and for the same reason: this is
third-party code we patch, and the patches need somewhere to live that is not
this repository.

Seeded from the 5.2.21 tarball rather than forked from a full upstream mirror.
bash releases as tarballs, so there is no upstream merge workflow to inherit,
and a small clean history is worth more than thirty years of import commits.
Moving to a later bash is then a vendor-drop commit, which is how the tarball
is published anyway.

## Recommended order

1. **Build it.** Vendor 5.2.21 with `--disable-readline`, a checked-in
   `config.h` per platform, its own meson archive, and the two `-D` renames
   (`main`, `wait_for`). Nothing about fork yet: the goal is a native bash that
   links, starts, and runs commands that do not fork.
2. Route the 32 kernel calls and add the 33 pure ones — required to link, and
   independently useful, since they are what any second native program will
   want rather than anything bash-specific.
3. Fix exec-in-place to keep the caller's pid (section 2). Independently
   useful, and it removes a whole class of surprise for a shell.
4. Re-launch fork, in the order the sites are worth doing: command substitution
   first (most common, most clearly one-way), then subshells, then pipelines,
   then coprocesses and process substitution.
5. Measure a real workload against emulated bash before deciding it is done.
   Section 0's 38–46x is the ceiling on the part of the time spent inside bash,
   not a prediction for any particular script.

Steps 1–3 are well-understood work of known shape. Step 4 is the research, and
it is where the estimate should be treated as soft.

## Where this got to (2026-08-15)

Steps 1, 2, 4 and 5 are done, and readline came back in rather than staying
out. Step 3 — exec-in-place keeping the caller's pid — is the one still open,
and is still not on the critical path: a login shell comes up, runs
`/etc/profile`, and behaves.

All seven `make_child` sites are converted. Six re-launch (subshells, command
substitution, pipeline elements, process substitution, the coprocess, the null
command); the seventh, the external command, spawns the argv bash already
computed, which is the one thing it must not re-run as text.

**Measured, against the guest's own emulated bash 5.2 rather than against host
bash, since that is the comparison a user actually makes:**

| | emulated | native | ratio |
|---|---|---|---|
| arithmetic loop, 20k iterations | 2.26 s | 0.12 s | **19x** |
| expansion loop, 4k iterations | 0.79 s | 0.04 s | **20x** |
| shell startup (`echo hi`) | 0.01 s | 0.01 s | — |

Lower than section 0's 38–46x, and honestly so: that measured host bash against
the emulator, where this measures native-inside-AOK, which pays for every
syscall through the shim. It is the number a script sees.

**Conformance**: bash's own 79-file test suite, run under each shell in turn,
gives 41 native against 42 emulated, with identical failure lists but for one
file — and the shared failures are the harness rather than either shell, byte
for byte the same output from both. See docs/bash_native_reentry.md.

Three things a shell needs that none of the above implies, and which took the
longest, are all in the seam rather than in bash: a spawned child must not
inherit the shim's own signal blocking, must be told its process group before it
execs, and must get SIGTSTP/SIGTTIN/SIGTTOU back at SIG_DFL. Without the first,
^C reached nothing; without the second, no foreground job owned the terminal;
without the third, ^Z did nothing at all.

### What is knowingly not the same

- **The job table does not cross a re-launch.** `jobs` in a pipeline or a
  command substitution — `jobs | grep`, `$(jobs -p)` — runs in a fresh shell
  that has no jobs. `jobs` itself, `fg`, `bg`, `kill %1` and `wait` are the
  parent's own and work.
- **Two native bash instances cannot run at once**, so the second one hands over
  to the guest's `/bin/bash`. Correct, and slower for that shell only.
- **A subshell is an emulated bash**, for the address-space reason in section 1.
  The interpretation the parent does — where a script's time actually goes —
  stays native.
