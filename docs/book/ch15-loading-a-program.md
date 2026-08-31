# 15. Loading a program

`execve` is the strangest syscall in Unix. It does not return. It keeps the
process — the pid, the parent, the open descriptors, the process group — and
replaces everything else: the address space, the code, the stack, the entry
point. A program that calls it ceases to exist while continuing to run.

It is also, in this system, the place where four separate stories meet: the
loader, the permission model, the security boundary that `setuid` depends on,
and the fork in the road where a path under `/AOK/native` stops being a file to
load and becomes a function to call.

## 15.1 The question `exec` asks

Before anything is loaded, `execve` has to decide whether this file may be
executed at all. The old implementation asked a question that was almost the
right one: it opened the file `O_RDONLY`, and then tested whether *any* execute
bit was set anywhere in the mode.

> **The bug that taught us this**
>
> One wrong question produced five distinct wrong behaviours in five different
> subsystems.
>
> **The file type was never checked**, because opening came first. A directory
> reached the ELF loader and came back with `EIO`. A FIFO reached `open(2)` —
> which *blocks until a writer arrives* — so `execve` of a fifo hung the task
> forever, with no way to distinguish it from a slow program. Linux answers
> `EACCES` for both.
>
> **Opening for read asks a different question from execute.** A 0644 file the
> caller could read was executed. A 0711 file — execute granted, read denied,
> which is the ordinary way to ship a binary nobody may copy — was refused.
>
> **"Any execute bit" is not a permission check.** A root-owned 0744 binary was
> executable by every user on the system, because the owner's execute bit was
> set and nobody asked whose bit it was.
>
> **`MS_NOEXEC` was decorative.** It was recorded on the mount and faithfully
> reported in `/proc/mounts`, and never once consulted, so `mount -o noexec`
> did nothing at all.
>
> **Capabilities survived an ordinary exec.** A process that lowered its uid
> while holding them — exactly what `prctl(PR_SET_KEEPCAPS)` plus `setresuid`
> is for — handed the full set to whatever it exec'd next.
>
> The fix is structural: factor the decision into `open_exec()`, following
> Linux's `do_open_execat` — check the type, ask for execute permission
> specifically, consult the mount flags, and only then open.

Two things about that entry are worth generalizing.

The first is that a single misplaced question can produce symptoms scattered
across a system, with no obvious relationship between them. "Executing a fifo
hangs" and "`mount -o noexec` does nothing" do not look like the same bug, and
they were.

The second is why it lasted so long, which is the subject of a rule this book
returns to: **root hides permission bugs.** AOK's command-line build runs as uid
0, and a root process skips permission checks entirely — so any bug in the
*order* of "permission check versus existence check", or in whose bits are being
tested, is invisible in every CLI session and every test run that way.

The clearest instance is not even in `exec`. `mkdir("/tmp")` returned `EACCES`
where Linux returns `EEXIST`, because AOK asked for write permission on the
parent before looking up the final component, and Linux's `filename_create` does
it the other way round. Since `mkdir -p` calls `mkdir` on every component and
treats `EEXIST` as success, **`mkdir -p /tmp/anything` failed outright for every
unprivileged user** — `/` is not writable by them and `/tmp` already exists. It
surfaced only because somebody on a real device was running as uid 1000.

Permission ordering is not testable as root. It has to be tested as somebody
else.

## 15.2 Two formats

Once the file may be executed, there are two things it can be.

**A `#!` script.** `shebang_exec` reads the first 128 bytes and parses
`#![spaces]interpreter[spaces]argument[spaces]` — one optional argument, as
Linux does, not the argument vector people often expect. The interpreter is then
executed, which means it faces exactly the same rules as any other executable.
That sentence is load-bearing: the older code opened the interpreter `O_RDONLY`,
so a script could run an interpreter the caller was not permitted to execute.
The permission model has to be applied at every place a program is chosen, not
just the one the user typed.

**An ELF image.** `elf_exec` maps the segments, and if the image names an
interpreter — which every dynamically linked program does — maps that too and
enters at *its* entry point, leaving `ld.so` to do the rest.

Around the mapping sit the details that make a real userland work: the load
bias for position-independent executables, the `brk` reservation set aside for
heap growth on the dynamic-PIE guests (Chapter 13), a guard against a later
`mmap` landing where the heap intends to grow, and the initial stack.

## 15.3 The stack the program wakes up on

A freshly `exec`'d program does not start with an empty stack. It starts with a
carefully constructed block: the argument strings, the environment strings, and
above them the auxiliary vector — a list of key/value pairs the kernel uses to
tell libc things it cannot otherwise discover.

Most `AT_*` entries are bookkeeping: where the program headers are, how large a
page is, what the entry point was, who the process is. Two are more interesting
here.

**`AT_HWCAP`** advertises the processor's features, and on aarch64 AOK
advertises *exactly the ISA features the JIT implements*. That is not a
formality. It is the same list that appears in `/proc/cpuinfo` (Chapter 1), and
it is what glibc's ifunc resolvers read when they choose which `memcpy` to use.
Claim a feature the translator does not implement and the guest's libc will
select an implementation full of instructions that do not run; claim too few and
it selects a slower one than necessary.

It also closes a loop from Chapter 8. `AT_HWCAP` determines which `memcpy`
variant the dynamic linker resolves, which determines the 64 bytes of prologue
sitting at that address — which is precisely why the HLE fingerprint table has
to be generated by running `dlsym` *inside the guest* rather than by reading the
shared object on disk. The aux vector this chapter builds is the input to the
fingerprints that chapter matches.

The riscv64 equivalent packs one bit per ISA letter, which is a pleasantly
direct encoding for a pleasantly regular architecture.

**`AT_SECURE`** is the security-relevant one. It tells libc that this execution
crossed a privilege boundary — a setuid or setgid binary, or a uid/euid mismatch
— and that it must therefore ignore `LD_PRELOAD`, `LD_LIBRARY_PATH` and every
other environment variable that could make it load code of the caller's
choosing.

It used to be hardcoded to zero, and the comment in `kernel/task.h` says what
that meant: "hardcoding it to 0 made every setuid-root binary loadable with an
attacker's shared object."

Getting it right required storing state that looks redundant until you see the
ordering:

```c
// What the credentials WILL be after the exec currently in progress, and
// whether it is a privileged (setuid/setgid) one. __do_execve fills these
// from the executable's stat immediately before the image is loaded, and
// elf_exec reads them when it builds the aux vector -- which is built
// before the real credential change below it, so it cannot just read the
// fields above.
```

The aux vector is constructed before the process's credentials actually change,
so at the moment `AT_SECURE` must be written, the task's own `uid` and `euid`
fields still describe the *old* program. The answer is to compute the future
credentials at the point the executable is stat'd, and carry them forward
explicitly. State that duplicates other state is usually a smell; state that
duplicates other state *at a different point in time* is often the only correct
design.

There is one more consequence of `AT_SECURE`, and Chapter 14 already told it
from the other end. Setting it routes musl into its secure-execution startup
path — which polls file descriptors 0, 1 and 2 and calls `a_crash()` if the poll
fails. So the day `poll` started answering `POLLNVAL` for host device nodes,
every setuid binary in the guest died with `SIGSEGV` before `main`. Two
independent pieces of correctness, each defensible alone, intersecting in a
crash that named neither.

## 15.4 Exec against the thread group

If a *non-leader* thread calls `execve`, Linux requires that exactly one thread
survive, and that it take over the group leader's identity. That is `de_thread`,
and Chapters 10 and 12 have already paid its bills:

- `exit_requested` exists because `SIGKILL` cannot mean "just this thread"; it
  flips the disposition while reusing all of the machinery that reaches a thread
  parked in an uninterruptible wait.
- `exit_finished` exists because `do_exit` leaves the group list partway
  through and keeps using its own struct afterwards, and `de_thread` needs to
  know when the old leader is genuinely finished with itself.
- And the family tree has to be repaired by hand, because threads here are
  children of their creator — which is how the exec'ing thread once became its
  own parent.

Three fields, in three different chapters, all from one syscall's requirement
that a process be able to shed its threads and keep its pid.

## 15.5 The fork in the road

The last thing `__do_execve` does before any of the above is ask whether the
resolved path is a natively-implemented program:

```c
// Natively-implemented programs (/AOK/native/*, kernel/native.h) are ...
```

If it is, the function sets `native_exec` on the task and returns before
reaching any of the ELF machinery. No segments are mapped, no interpreter is
loaded, no entry point is set. When the task would otherwise begin executing the
loaded image, it calls a C function inside iSH-AOK instead. Part V is about what
that means; here it is enough to note where the branch is, and that it is a
branch in `execve` rather than a special case bolted on elsewhere — which is
exactly why the caller cannot tell.

Even so, exec's ordinary obligations do not go away, and forgetting them was a
source of bugs. `exec_apply_native_process_state` resets the signal
dispositions that `execve` must reset — and has to reset them in **two** places,
because the shim keeps native code's own view of the dispositions beside the
guest table, and "resetting one and not the other" left a native program running
under the previous program's handlers. It also unblocks signals that would
otherwise stay blocked for the native program's entire run.

There is a sharper trap recorded in the same file, and it is the one that makes
this a chapter rather than a footnote: a program that resolves a helper — a
pager, an editor, a shell — can land on a native program without meaning to, and
early versions of that path could take down the whole application. Chapter 25
tells that story properly.

## 15.6 What crosses the boundary

`exec` is the one moment where a process can change what it is allowed to do, so
it is worth listing what survives it and what does not.

**Survives:** the pid, the parent, the process group and session, open file
descriptors without `FD_CLOEXEC`, the current directory, the root directory,
resource limits, the controlling terminal, and — where the ambient set says so —
capabilities.

**Does not survive:** the address space in its entirety, threads other than the
one calling, signal *handlers* (though the blocked mask does survive, which
catches people out), pending alternate signal stacks, memory mappings including
shared ones, and the JIT's translations, since a new address space gets a fresh
`struct jit` (Chapter 6).

**Changes:** the credentials, if the binary is setuid or setgid; `AT_SECURE`
accordingly; and the capability sets, which are now actually recomputed rather
than inherited wholesale.

That list is the security boundary. Before the work described in this chapter,
three of its lines were wrong at once — capabilities crossed when they should
not, `AT_SECURE` never said so, and the execute permission that gates the whole
thing was being tested against the wrong bits.

## 15.7 Closing Part III

Six chapters of kernel, and one thing runs through all of them.

Every subsystem here implements a contract that userspace can *observe*: a
return value, an errno, a `/proc` file, a field in a structure, a signal
arriving or not arriving. And the failures were almost never "the code does the
wrong thing". They were "the code answers a slightly different question from the
one that was asked" — read permission instead of execute, the wake count instead
of the expected value, the device's idle time instead of this process's, a
pointer-sized argument instead of a 32-bit one, the host's readiness model
instead of Linux's.

That is the characteristic bug of an emulated kernel, and it is why Chapter 9's
oracles matter more here than test coverage does. You cannot test your way to
the right question. You can only ask a real Linux what it does, and then check
that you do the same.

Part IV goes down one more level, to the layer where the guest's questions stop
being about processes and start being about files — and where the answers have
to be manufactured out of a sandbox that has no owners, no modes, and no device
nodes at all.

---

*Anchors:* [kernel/exec.c](../../kernel/exec.c), [kernel/elf.h](../../kernel/elf.h),
[kernel/task.h](../../kernel/task.h) (`exec_secure`, `exec_auxv_*`),
[kernel/vdso.c](../../kernel/vdso.c), [vdso/](../../vdso),
[kernel/native.h](../../kernel/native.h), [fs/path.c](../../fs/path.c),
[fs/mount.c](../../fs/mount.c), `tests/manual/` (the exec and permission suites).

*Story:* one wrong question in `execve` — read permission instead of execute,
asked before the file type was checked — producing five unrelated-looking
failures, including `execve` of a fifo hanging forever and `mount -o noexec`
doing nothing at all.
