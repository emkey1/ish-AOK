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

### The state script has ordering rules

The script a re-launched child runs is built by `aok_serialize_state`, and
three of its rules are load-bearing rather than tidy. They are in the code with
their reasons; the short form:

- **extglob must be ON while the functions are emitted**, whatever the parent's
  own setting is. A function body containing `?(...)` can only be re-read by a
  shell that has it, bash prints such functions back in that syntax, and
  bash-completion turns extglob off again after defining hundreds of them. Get
  this wrong and every child dies on a syntax error partway through the state,
  which shows up as every command substitution in a login shell coming back
  empty.
- **shopt before `set -o`**, because `shopt -u extdebug` turns off `-E` and `-T`.
- **errexit and nounset last**, after the variables the state assigns.

What must NOT cross matters as much: a forked child resets its traps
(`reset_signal_handlers`), keeping only the ignored ones and DEBUG/ERR/RETURN,
so sending the EXIT trap made it fire once per subshell and once per command
substitution. `AOK_BASH_DUMP_STATE=1` prints what a child was handed, and is
the first thing to reach for when one misbehaves.

### What is knowingly not the same

- **The job table does not cross a re-launch.** `jobs` in a pipeline or a
  command substitution — `jobs | grep`, `$(jobs -p)` — runs in a fresh shell
  that has no jobs. `jobs` itself, `fg`, `bg`, `kill %1` and `wait` are the
  parent's own and work.
- **`bind` is shared between live shells** — see the TLS section below for why,
  and for what is no longer shared.
- **`BASH_SUBSHELL` is 0 in a subshell**, where bash counts the nesting. A
  re-launched child is a fresh top-level shell and has nothing to count from;
  the state script cannot carry it, because `BASH_SUBSHELL` is readonly and the
  child sets its own.
- **`PPID` in a subshell is the parent shell**, where bash keeps the
  grandparent — same reason, and the same readonly obstacle.
- **`trap -p` inside a child under-reports**: bash lists the strings of traps
  that are not armed there, and reproducing that would mean setting a trap and
  disarming it, which an early-exiting child never reaches.

## Many shells at once: thread-local globals (2026-08-16)

A native program is a C function on a guest task's thread inside the app's one
address space, so until now two live native bash instances would have shared
bash's globals — the same wall `fork` hits. `kernel/bash_glue.c` therefore
allowed exactly one, and every other shell in the app silently got the emulated
bash instead. That is gone. Nested shells and concurrent shells both work, each
with its own variables, functions, options, jobs and readline state.

What made it affordable is that the state which has to differ per shell is
small: about 50 KB of `__data`, `__bss` and `__common` across the whole
bash+readline archive. bash's own malloc is not compiled in, so there is no
arena to duplicate. Making each of those variables `__thread` gives every task
its own copy. Measured on this platform TLS access costs nothing in a loop —
clang hoists the `$tlv$init` resolution out — and the 19–20x numbers above are
unchanged.

The conversion is tooling rather than a patch, so moving to a later bash is a
re-run rather than a re-do:

| tool | what it converts | driven by |
|---|---|---|
| `tools/bash-tls-rewrite.py` | declarations and definitions in the vendored tree | clang `-ast-dump=json` |
| `tools/bash-tls-fix-defs.py` | the `.def` files the AST cannot reach | compiler diagnostics |
| `tools/bash-tls-fix-tables.py` | 7 option tables, to per-thread `aok_fix_*()` fixups | hand-listed |
| `tools/bash-tls-fix-statics.py` | file-local statics | `nm` |
| `tools/bash-tls-fix-externs.py` | whatever is still shared, by name, everywhere | `nm` |
| `tools/check-bash-tls.py` | **the gate** | `nm` + relocations |

### The gate is the important part

An incomplete conversion does not fail to build. A translation unit that
declares one of these variables without `__thread` compiles clean, links clean,
and reads the wrong memory: measured on a two-file test, a variable holding 1
read back as `-1882127480`. In bash it was worse than wrong values —
`dist_version` is defined `const char * const` in version.c but declared plain
`char *` in shell.h, so only the declaration got `__thread`, and reading it
treated the string's first bytes as a thread-vector descriptor and called
through it. The shell jumped into the text of `"5.2"`.

`check-bash-tls.py` asks two questions per object file. The first is whether any
symbol is thread-local in one place and ordinary data in another, in either
definitions or relocations. The second, and the one that matters more, is
simply: **what is in a writable data section and not thread-local?** The
mismatch checks only fire when two files disagree; a variable that no file ever
converted is something every file agrees on — consistently shared, and
consistently wrong once two shells are live. That question found 129 file-local
statics and 13 externals the AST-based pass had silently skipped, including
`o_options`, one of the tables each thread fills in with its own addresses.
Anything allowed to stay shared is in `ALLOW` in `bash-tls-fix-externs.py`,
where it has to be justified in writing, and the other tools import that same
set so they cannot undo each other.

### What is still shared, and why

- **readline's keymaps.** They cross-reference each other in static
  initialisers — `emacs_standard_keymap`'s Control-x entry *is*
  `emacs_ctlx_keymap` — so they cannot be thread-local without a fixup pass over
  all of them. The visible consequence is narrow and interactive: `bind` in one
  live shell is seen by another. `_rl_keymap` and the other keymap *pointers*
  are per-shell, so which keymap a shell is using is its own business.
- **The builtins table.** Identical in every shell, since it is the set bash was
  compiled with. `enable`/`disable` flip flags inside it, and that is shared.
- **Read-only lookup tables that upstream simply did not mark `const`** —
  `default_prefixes`, `posix_collsyms` and friends.

Everything else — including `xpg_echo`, `localvar_unset`, `shell_function_defs`,
the completion state and all five option tables — is per shell.

### If you re-run the tools

`bash-tls-rewrite.py` is the least trustworthy of them. clang's JSON omits a
location's file whenever it repeats the previous one, and macro locations carry
theirs inside nested `spellingLoc`/`expansionLoc` dicts, so the walk drifts and
starts attributing variables.c's offsets to pcomplete.h. Its word-boundary guard
rejects the bad ones rather than corrupting a header, which is why the drift is
survivable — but it is also why it silently skipped 129 statics. The `nm`-driven
tools have nothing to drift and should be trusted over it. Run the gate after
any of them.

## Native subshells, and what they actually cost (2026-08-16)

With bash's globals thread-local, the premise that forced a re-launched child to
be the guest's emulated `/bin/bash` no longer holds: a live parent and a live
child no longer collide. `AOK_SUBSHELL_BASH` is `/AOK/native/bash` now. That
fixes `$$`, which was previously listed here as structural — the child computed
its own pid, so the universal `file-$$` idiom silently disagreed across the
boundary and `mkfifo /tmp/f-$$; cmd > /tmp/f-$$ &` deadlocked on a FIFO nobody
would ever open. bash's own `tests/source6.sub` is the case that found it.

**The performance result is not the one that was predicted, and the difference
is worth recording.** Measured on one build, in the same guest:

| | native | emulated | |
|---|---|---|---|
| arithmetic loop, 20k | 87 ms | 1438 ms | **16.5x faster** |
| 30 command substitutions | 294 ms | 100 ms | 2.9x *slower* |
| 30 subshells | 275 ms | 82 ms | 3.4x *slower* |
| 30 bare shell starts | 84 ms | 335 ms | 4x faster |

**Then the slow part was found, and it was none of the obvious candidates.**
A subshell cost 8.3 ms while the spawn and a full native bash startup together
cost 1.8 ms, and the state script grew 2.7x for only +0.7 ms — so it was not the
spawn, not bash's startup, and not parsing. It was that every line of the state
carried its own `2>/dev/null`, each one an open/dup2/close through the shim,
about 85 times per subshell. 120 such lines cost 9.2 ms; the same lines under
one `exec` redirection cost 1.4 ms. After the fix:

| | native | emulated | |
|---|---|---|---|
| 30 subshells | **83 ms** | 78 ms | parity |
| 30 command substitutions | **79 ms** | 90 ms | native *faster* |

Note it must be `exec`, not a `{ ... } 2>/dev/null` wrapper: bash parses a `-c`
string command by command, and a brace group is one command, so the `shopt -s
extglob` inside it would not have run before the function bodies inside it were
parsed.

So the change made the re-launch path about **4x cheaper** — ~11.2 ms per
subshell start became ~2.8 ms — and subshells roughly twice as fast as they
were. It did **not** make them faster than the emulated shell, which forks
through AOK's `sys_clone`: native C that was never emulated, at ~2.7 ms against
a re-launch's ~9.2 ms all in.

This was predicted by section 0 and then talked past anyway. The proposal
claimed native subshells would be "19–20x faster", by extrapolating the
*interpretation* speedup onto the *re-launch* path — two entirely different
mechanisms. The rule that falls out: **the win is interpretation, the loss is
forking.** A script whose time goes into expansion and arithmetic gets much
faster; a script that spawns a subshell per loop iteration gets slower. Which
one a real workload looks like is still the open question section 0 asked, and
still the thing to measure before claiming an end-to-end number.

Two divergences are *new* with this change, both verified against the emulated
shell rather than assumed:

- **`BASH_SUBSHELL` stays 0** at every nesting depth, where bash counts 0/1/2.
  A re-launched child is a fresh top-level shell with nothing to count from, and
  the variable is readonly so the state script cannot carry it.
- **`PPID` in a subshell is the parent shell**, where bash keeps the
  grandparent — same readonly obstacle.
