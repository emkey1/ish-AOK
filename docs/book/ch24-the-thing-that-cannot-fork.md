# 24. The thing that cannot fork

A shell without `fork` is not a shell.

Command substitution is a fork. A pipeline is a fork per stage. A subshell is a
fork by definition. Background jobs, process substitution, `(cd /tmp && ls)` —
all fork. Take it away and what remains is a command launcher, not a shell, and
it will fail on the first line of almost any real script.

Chapter 22 established that a native program cannot fork: it is a C function on
a guest task's thread, and a real fork's child continues on a *copy of the
parent's C stack*, which is host memory at absolute host addresses that no
translation scheme reaches.

So compiling bash into the application looked like a project that could not
work. This chapter is about why it did, and it opens with a measurement that
inverted the plan.

## 24.1 The measurement that changed the design

The original design document dismissed re-launch — spawning a fresh shell and
handing it the parent's state — as the expensive option, and went looking for
something cleverer. Then somebody measured the pieces:

| | emulated in AOK | host (native ceiling) | ratio |
|---|---|---|---|
| arithmetic loop, 20k iterations | 4.17 s cpu | 0.09 s cpu | **46x** |
| string/expansion loop, 4k iterations | 29.3 s cpu | 0.78 s cpu | **38x** |
| one `( : )` subshell | ~2.5 ms | ~0.7 ms | 3.5x |
| one bash startup | ~15 ms | ~3.6 ms | 4x |
| one **native** task spawn (`smallclue true`) | **~1.6 ms** | — | — |
| one emulated ELF exec (`/bin/true`) | ~7.4 ms | — | — |

Two conclusions fall out, and they point the same way.

**Interpretation is what emulation costs you: 38–46x.** That is bash's parser,
word expansion and arithmetic being translated one instruction at a time
(Chapter 6), and it is exactly what compiling bash natively removes.

**Forking is not.** A guest `fork` costs about 2.5 ms — but that cost is AOK's
`sys_clone`, which is native C inside the emulator and was *never being
emulated*. There is no 40x penalty to recover there, because there was no
penalty.

So replacing bash's `fork` with "spawn a fresh native bash and give it the
state" costs roughly what bash already pays, and possibly less: a native task
spawn is 1.6 ms against an emulated ELF exec's 7.4 ms.

The document's own conclusion about itself:

> That inverts the conclusion this document originally reached. The re-launch
> design was dismissed as the expensive option; it is the affordable one,
> because the expensive thing was never fork.

With an honest qualifier attached, which is why the shipped figure quoted in
Chapter 22 is roughly 16x rather than 46x: the ceiling above compares a *host*
binary against an emulated one, while the shipped native program reaches its I/O
through the shim and stops at a checkpoint on every read and write (Chapter 23).
And 38–46x applies only to time spent *inside* bash — external commands stay
emulated unless they are native applets, so an end-to-end figure for a real
script is lower and depends entirely on the mix.

## 24.2 Serialize, re-launch, become

The mechanism, in `deps/bash/aok_fork.c` and its zsh counterpart: when the shell
would fork, it instead builds a **script** describing its own state, spawns a
fresh `/AOK/native/bash -c`, and lets the child run that script to become what
the forked child would have been.

Variables, functions, aliases, shell options, the working directory, the
descriptor state — all of it emitted as shell commands that a shell can read.

Getting the *content* right is the easy half. The order decides whether the
child can parse it at all, and every rule below was learned from a bug.

Before any of them, though, the tool that made them findable:

> **Set `AOK_BASH_DUMP_STATE=1` and read what the child was handed.** Three of
> the four defects below were invisible in the parent and obvious in one look at
> that dump. It is the first thing to do, not the last.

That is a general principle for anything that generates code: the fastest path
to a generator bug is reading its output, and the cost of making the output
visible is one environment variable.

## 24.3 Four ordering rules

**1. `extglob` must be on while function definitions are emitted, whatever the
parent's own setting is.**

bash stores functions as parse trees and prints them back out in bash's
syntax — so a function body containing `?(...)` or `+(...)` can only be re-read
by a shell that has `extglob` on. And `bash-completion` defines hundreds of such
functions and then *turns `extglob` off again*.

So a login shell sits there, `extglob` off, holding a table of functions that
cannot be re-read. Every child died on `syntax error near unexpected token (`,
which left **every command substitution in a login shell empty**. Not failing —
empty, which is worse, because `$(...)` returning nothing is a valid thing for a
script to see.

The fix is to force it on for the function block and restore the parent's value
afterwards, with the other options.

**2. `shopt` before `set -o`.**

`shopt -u extdebug` calls `shopt_set_debug_mode`, which does
`error_trace_mode = function_trace_mode = debugging_mode` — it turns *off* `-E`
and `-T`. So a state script that sets `set -o` flags first and then issues
`shopt` commands silently undoes two of them.

The reason bash's own startup never hits this is worth the detour: it does
`SHELLOPTS` then `BASHOPTS`, and `BASHOPTS` lists only options that are **on**,
so it never issues a `shopt -u` at all. A state script that faithfully describes
every option in both directions does. Faithfulness exposed a path bash's own
code never takes.

**3. `errexit` and `nounset` go last**, after the variables the state assigns.
Otherwise the state script trips over its own assignments.

**4. `bash -c` parses incrementally, command by command.** This is what makes
rules 1 and 2 work at all: a `shopt -s extglob` earlier in the string is in
effect before a later function definition is *parsed*, not merely before it is
run. Verified directly, and load-bearing — Section 24.5 shows what happens when
a change accidentally violates it.

## 24.4 What must not cross

A re-launch is not a fork, and the child must end up with the state a *forked*
child would have had — which is not the same as the state the parent holds.

Traps are the clearest case. A forked child in real bash calls
`reset_signal_handlers()` before anything else, so caught signals return to
their defaults. A state script that faithfully transferred the parent's traps
would produce a child more faithful to the parent than a real fork is, and
therefore wrong.

The general form of that rule is worth stating, because it is easy to get
backwards: **the target is not the parent's state, it is the state a real
`fork` would have produced.** Anything `fork` resets must be reset here too, and
that means reading what the shell does on the child side of a fork, not just
what it holds on the parent side.

## 24.5 Where the cost actually was

Once it worked, a subshell cost 8.3 ms, which is too slow. The obvious suspects
were the spawn, bash's startup, and parsing a long state script.

All three were measured, and all three were wrong. The spawn *and* a full native
bash startup together cost 1.8 ms. Growing the state script 2.7x added 0.7 ms.

The cost was that **every line of the state script carried its own
`2>/dev/null`** — about 85 of them per subshell, each an open, a dup2 and a
close through the shim (Chapter 23). Measured directly: 120 such lines took
9.2 ms; the identical lines under a single `exec` redirection took 1.4 ms.

The fix is to redirect once — `exec {__aok_stderr}>&2 2>/dev/null` — and restore
before the command runs. And it carries a sting that is the whole of Section
24.3 arriving through a different door:

> It must be `exec`, not `{ ... } 2>/dev/null` — bash parses a `-c` string
> command by command, and a brace group is ONE command, so the
> `shopt -s extglob` inside it would not run before the function bodies inside
> it were parsed.

The brace group is the tidier-looking construct and it silently reintroduces the
empty-command-substitution bug from rule 1. Rule 4 is not a curiosity; it is a
constraint that any change to the state script has to be checked against.

The result: re-launch about **4x cheaper**, 11.2 ms per subshell start down to
2.8 ms. And the honest scoring:

| | native | emulated | |
|---|--:|--:|---|
| 30 subshells | 83 ms | 78 ms | parity |
| 30 command substitutions | 79 ms | 90 ms | native faster |

It did *not* beat the emulated shell at forking, and the document says so: the
emulated shell forks through `sys_clone`, native C at ~2.7 ms, against a
re-launch's ~9.2 ms all in. Parity was the goal and parity is what was reached.
The 46x is elsewhere — in every line of script that is *interpreted* rather than
forked, which is most of them.

## 24.6 Many shells at once

The first native bash could have exactly one live instance, because bash keeps
its state in globals and there is only one copy of a global in an address space.
One shell is not a shell either: a pipeline needs several, a subshell inside a
command substitution needs nesting.

The answer was to make bash's mutable globals `__thread`, mechanically, with a
set of scripts (`tools/bash-tls-*.py`). The conversion is not the interesting
part. The five ways it silently half-worked are.

> **The bug that taught us this**
>
> **A mismatch gate is not a coverage gate.**
>
> `tools/check-bash-tls.py` originally compared thread-local against ordinary
> definitions per symbol, and fired when two files disagreed. That finds
> half-converted variables — and it is structurally blind to the worst case,
> because *a variable no file ever converted is something every file agrees
> on*: consistently shared, consistently wrong.
>
> Asking a second and completely different question — "what is in a writable
> data section and not thread-local?" — found **129 file-local statics and 13
> externals** the conversion pass had skipped, including `o_options`, which is
> one of the per-thread fixup tables.
>
> The rule, stated generally: **whenever a gate checks consistency, ask
> separately whether the thing was done at all.**

Two more from the same work, both about tools lying:

`nm -m` puts an alignment field between the section and the linkage word for
common symbols — `(common) (alignment 2^3) external _foo`. A regex matching
`\(common\) external` therefore matched nothing, and cheerfully reported
"nothing shared" while seven symbols were. Match the fields separately.

And a C subtlety: `version.c` defines `const char * const dist_version` while
`shell.h` declares plain `char *dist_version`. Only the declaration received
`__thread`, so reads went through a thread-local slot that was never the
definition — treating the string's first bytes as a pointer.

## 24.7 The exec stand-in

`exec` is the other primitive a native program cannot have. A C function cannot
become a guest image halfway through itself, so `nlibc_execve` **spawns the
program and waits for it**.

That substitution has a consequence nobody drew until it broke:

> `sh -c 'sleep 5'` execs in place, so **the pid a shell knows for that job IS
> that wait**, and every signal aimed at the job hits the stand-in, never the
> program.

> **The bug that taught us this**
>
> `wait` on a signal-killed background job returned **127** — a shell's "no such
> job" status — under native zsh *and* native bash.
>
> Neither shell was at fault. `native_waitpid` returned `EINTR`, the stand-in
> read that as "child gone", and exited 127.
>
> The diagnostic move worth keeping: **`SIGKILL` producing the same 127 is the
> tell.** It rules out every status-encoding theory and every signal-numbering
> theory at once — `SIGKILL` cannot be caught, mishandled or renumbered — and
> points squarely at the waiter.

Three rules came out of it, each one a bug that was there:

1. **`EINTR` must not end the wait.** Loop: forward the signal, checkpoint,
   retry.
2. **Exit with the child's raw guest status word**, which `do_exit_group` takes
   verbatim. The tempting `128 + N` prints the same `$?` and lies to everything
   that reads `WIFSIGNALED`.
3. **Reset its own caught handlers to `SIG_DFL`, keeping `SIG_IGN`** — a real
   `exec` does exactly that, and keeping `SIG_IGN` is what makes `nohup` work.
   Without it, the shell's own `SIGCHLD` reaper is still installed, and the next
   checkpoint lets it reap the exec'd program out from under the stand-in.

## 24.8 Scoring it honestly

The zsh port ships 119 differential cases at
`/AOK/tests/native_zsh_fork_state.sh`, **with every expectation taken from what
real zsh prints** rather than from what looked reasonable. 116 pass.

The two failures are process substitution — `<(...)` and `>(...)` — and the
project's account of them is a good example of the discipline in Chapter 9. That
is a property of the *rootfs*, not of the shell: it needs `/dev/fd`, which the
Alpine image does not provide, so it fails identically under the emulated
`/bin/bash` there, and works under both shells on Devuan, where `/dev/fd` is a
symlink to `/proc/self/fd`.

Two further gaps *are* the shell's, and are written down as such:

- A pattern is compiled at first use and cached in the parse tree, with nothing
  recording the options in force at the time — so a re-launched child can
  compile it under different options than its parent did.
- `pipestatus` under a MULTIOS redirection reports `1 0` where zsh reports
  `0 0`.

MULTIOS itself needed a second native program, `zsh-multio`, "because the
descriptors have to be held by something that is not the shell" — a redirection
that fans one stream into several needs a process that outlives the command, and
a native program is a function call that does not.

Here is what the result looks like, which is the point of all of it:

```
% echo $(echo A); echo B | tr B C; (echo D); sleep 0.1 & wait; echo E
A
C
D
E
```

Command substitution, a pipeline, a subshell and a background job, in one line,
in a shell that cannot fork.

## 24.9 What re-launch teaches

The instinct when a primitive is unavailable is to ask how to emulate it. That
question had an answer here and the answer was no: Chapter 22's measurement
showed that even a guest address space for native programs would not produce
`fork`, because the C stack is unreachable.

The productive question turned out to be the other one: **what did callers
actually need from it?** And what bash needs from `fork` is not a duplicated
address space. It is a second shell that agrees with this one about variables,
functions, options and descriptors, and then diverges.

That is state transfer, and state transfer can be done with a text file.

The cost of the substitution is not zero and the book does not pretend it is:
subshells reach parity rather than beating emulation, traps need special
handling, process substitution needs `/dev/fd`, and there are two known
divergences with tests pinning them. But the shell is a shell, the arithmetic is
46x faster, and the thing that made it possible was measuring the primitive
everyone assumed was expensive and finding out that it never was.

---

*Anchors:* `deps/bash/aok_fork.c`, `deps/zsh/Src/aok_fork.c`,
[kernel/bash_glue.c](../../kernel/bash_glue.c),
[kernel/zsh_glue.c](../../kernel/zsh_glue.c),
[kernel/native_libc.c](../../kernel/native_libc.c) (`nlibc_exec_standin`),
[docs/bash_native_plan.md](../../docs/bash_native_plan.md),
[docs/bash_native_reentry.md](../../docs/bash_native_reentry.md),
`tools/bash-tls-*.py`, `tools/check-bash-tls.py`, `tools/zsh-tls-fix-tables.py`,
`tests/manual/native_zsh_fork_state.sh`,
`tests/manual/native_bash_fork_state.sh`.

*Story:* every command substitution in a login shell coming back empty — because
`bash-completion` defines hundreds of functions containing `?(...)`, then turns
`extglob` off, and a state script written in the parent's own settings hands the
child a table of functions it cannot parse.
