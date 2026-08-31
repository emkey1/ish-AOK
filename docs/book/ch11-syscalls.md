# 11. Syscalls

A syscall is where the guest stops being a program and becomes a client. Up to
that instruction, everything it did was arithmetic on its own state, and the
emulator's only obligation was to compute the same answers the hardware would.
At the syscall boundary the guest asks a question that only an operating system
can answer, and iSH-AOK has to be that operating system.

`kernel/calls.c` is 6,417 lines long and it is the front door. This chapter is
about what happens between the guest's `svc #0` and the value that lands back in
`x0`.

## 11.1 One funnel

Every exceptional thing a guest can do arrives at the same function:

```c
void handle_interrupt(int interrupt) {
    switch (interrupt) {
        case INT_SYSCALL:        handle_syscall_interrupt(cpu); break;
        case INT_AMD64_SYSCALL:  handle_amd64_syscall_interrupt(cpu); break;
        case INT_ARM64_SVC:      handle_arm64_syscall_interrupt(cpu); break;
        case INT_RISCV64_ECALL:  handle_riscv64_syscall_interrupt(cpu); break;
        case INT_PF:             handle_page_fault_interrupt(cpu); break;
        case INT_GPF:            handle_general_protection_interrupt(cpu); break;
        case INT_BUS:            handle_bus_interrupt(cpu); break;
        case INT_UNDEFINED:      handle_illegal_instruction_interrupt(cpu); break;
        case INT_DIV:            handle_arithmetic_interrupt(cpu); break;
        case INT_PRIV:           handle_privileged_instruction_interrupt(cpu); break;
        case INT_BREAKPOINT:     /* SIGTRAP, TRAP_BRKPT */ break;
        case INT_DEBUG:          /* SIGTRAP, TRAP_TRACE */ break;
        case INT_TIMER:          handle_timer_interrupt(cpu); break;
    }
}
```

Chapter 6 described how execution leaves translated code; this is where it
arrives. The four syscall cases correspond to four instructions — i386's
`int $0x80`, amd64's `syscall`, arm64's `svc`, riscv64's `ecall` — and they
converge almost immediately.

Each per-ABI entry point does one thing before converging, and it is worth
noticing because it is cheap and catches a whole bug class:

```c
if (current->abi != GUEST_ABI_ARM64) {
    printk("ERROR: %d(%s) arm64 SVC in non-arm64 task at pc=%#llx\n", ...);
    deliver_signal(current, SIGILL_, info);
    return;
}
```

An `svc` executed by a task whose ABI is not arm64 means something upstream is
badly wrong — a mis-dispatched engine, a corrupted task, a guest jumping into
foreign code. The check costs one comparison, delivers `SIGILL` to the guest
rather than corrupting state, and prints a line naming the pid, the command, and
the program counter. Invariants at boundaries, stated as code.

## 11.2 The shape of a dispatch

`handle_syscall_interrupt` is the common path, and its steps are:

1. Look up the `syscall_abi_dispatch` for this task's ABI (Chapter 7).
2. If the task is traced and stopped at syscall entry, run the ptrace stop —
   **before** decoding anything.
3. Read the syscall number through the ABI's accessor.
4. Intercept the private accelerator numbers.
5. Range-check the number against the table size.
6. Look up the handler; a NULL entry logs and returns `ENOSYS`.
7. Read the arguments through the ABI's accessor.
8. Marshal them, if this handler takes the legacy 32-bit form.
9. Call the handler.
10. Write the result back through the ABI's accessor.

Step 2's ordering is subtle enough to have its own comment:

> The tracer can rewrite the live register file at syscall-entry stops, so
> decode the syscall only after the tracee is resumed.

`strace -e inject` and every syscall-rewriting debugger depends on that. Read
the number first and you have decided what the syscall is before the tracer has
had its say, and the rewrite silently does nothing.

## 11.3 `ENOSYS` or `SIGSYS`: what "not implemented" looks like

Linux answers an unknown syscall number with `ENOSYS`. AOK answers with
`SIGSYS`:

```c
if (syscall_num >= dispatch->num_syscalls) {
    printk("ERROR: %d(%s) missing %s syscall %d\n", ...);
    deliver_signal(current, SIGSYS_, SIGINFO_NIL);
    return;
}
```

That is a deliberate divergence and a defensible one — a guest making a syscall
this build has never heard of is usually a sign of something worth investigating
loudly, and `SIGSYS` produces "Bad system call" plus a log line, where `ENOSYS`
produces a return value nobody prints.

It is also a decision with consequences, and one of them cost real work.

> **The bug that taught us this**
>
> The two accelerator syscalls (Chapter 33) live above every real syscall range
> — `ISH_SYS_AEAD` is `0xacc0`, `ISH_SYS_PIXOP` is `0xacc1` — so they fail the
> range check and must be intercepted before it. They were originally wired for
> the arm64 and riscv64 guests only.
>
> The intended consequence was that x86 guests would not get the accelerators.
> The actual consequence was worse than that. Every guest-side consumer of these
> — the pixman `LD_PRELOAD` shim, the OpenSSL provider, the conformance test —
> has the same fallback story: *probe once, and on `ENOSYS` pass everything
> through*. That is exactly right on Linux. Here the probe did not return
> `ENOSYS`; it delivered `SIGSYS`, and the process died at the probe rather than
> falling back.
>
> Making a capability unavailable is fine. Making it *fatal to ask about* is
> not, and the difference is entirely in what an unimplemented call looks like
> to the caller. The interception now runs for every ABI.

Below the range check, "not implemented" has three more spellings, and each is a
statement about the caller:

- **A NULL table entry** logs a stub-syscall line naming pid and command, and
  returns `ENOSYS`. The number is known and nobody has written it.
- **`syscall_stub`** is the same thing with an explicit entry, for syscalls that
  are deliberately absent.
- **`syscall_stub_silent`** returns `ENOSYS` without logging, and the entries
  using it carry their justification: `io_uring_setup`, `io_uring_enter`,
  `io_uring_register` ("so liburing/cmake fall back to epoll"), `open_tree`,
  `fspick`, `mount_setattr` ("util-linux falls back to `mount(2)` on ENOSYS"),
  `pidfd_getfd`, `process_madvise`.

The silent set is the interesting one. Those are calls that well-written modern
software probes on startup and handles the absence of. Logging them would
produce a line per process for a non-event. The judgement being made is: *this
caller knows how to be told no*, which is exactly the judgement Chapter 40 says
you must not make wrongly in the other direction.

## 11.4 The legacy marshaller

Most syscall implementations in `kernel/` take `dword_t` arguments — 32 bits,
because for nine years the only guest was i386. When the amd64 guest arrived,
rewriting every handler to take 64-bit arguments was not on the table, so 64-bit
guests' arguments are packed down into the 32-bit form for handlers that have
not been converted.

That is `marshal_syscall_args_legacy`, and it is a per-syscall table of arities
and packings. `truncate` takes a path and a 64-bit length, which becomes a path
and two dwords. `fallocate` takes an fd, a mode, and two 64-bit values, which
becomes six dwords exactly filling the argument array. `clone` reorders two
arguments, because i386's `clone` has a different parameter order from the
asm-generic one.

The safety rule reads well:

> All 64-bit ABIs validate: a 64-bit value reaching the 32-bit marshalling is a
> dispatch bug, and failing loudly here (SIGSYS) beats silently truncating a
> pointer (how arm64 `dmesg` broke).

Truncating a pointer produces a call that acts on the wrong memory and returns
success. Failing loudly produces a bug report. Given a choice between those, the
loud one is right.

> **The bug that taught us this**
>
> The check cannot do what it claims. It tests whether the value in the argument
> register fits in 32 bits — and it cannot distinguish a real 64-bit pointer
> from a legitimate 32-bit `int` argument whose upper half happens to hold
> junk.
>
> Both 64-bit psABIs leave the upper 32 bits of a register carrying a 32-bit
> argument **architecturally undefined**, and Linux simply truncates to the
> declared parameter type. A compiler is entitled to leave anything up there.
>
> So the check killed valid programs, and it did so intermittently, depending on
> what the compiler had left in a register. The file's own comments record the
> long tail of one-off "fix this syscall's arity" patches it produced: `cp -p`,
> `login`, `man-db`, `mount`, `stress-ng`, `syslog-ng`, `useradd`, `dpkg`/`apt`.
>
> Then in July 2026 it broke `uv venv` on aarch64. LLVM had loaded a file
> descriptor as a whole 64-bit stack word before calling `fchmod(fd, 0666)`. The
> fd was a small positive integer; the upper half was whatever else had been on
> the stack. "Bad system call", and a Python tool that would not create a virtual
> environment.
>
> The fix is `syscall_legacy_args_are_scalars()`, which skips the check entirely
> for syscalls whose arguments are all 32-bit scalars — the case where the upper
> half is *architecturally* meaningless and there is nothing to validate.
>
> The operational note that came with it is the durable part. The scalar lists
> are exhaustive for what reaches the legacy marshaller today; everything else
> is dispatched full-width. So a **new** entry appearing there means somebody
> added a syscall to a legacy table, and the right fix is usually a native
> full-width handler rather than another arity tweak. When a guest dies with
> "Bad system call", read the fd-555 log for `needs full-width args` — which now
> prints the arguments — before suspecting the JIT.

## 11.5 Full-width dispatch, and why a table entry can be inert

The long-term direction is away from the legacy path. `handle_asm_generic_native_syscall`
and `handle_amd64_native_memory_syscall` run before the table for a growing set
of numbers, taking arguments at their real width and calling handlers written
for 64 bits.

This produces the structure Chapter 7 warned about, and here is the mechanism
behind the warning. A syscall dispatched natively still has a table entry:

```c
[229] = (syscall_t) sys_munlock, // dispatched natively (full-width); entry needed to pass the NULL check
```

The entry exists so that step 6 of Section 11.2 does not treat the number as
unimplemented. It is never called. Editing it changes nothing, and reading the
table to find out what a syscall does will mislead you for exactly as long as
you trust it.

There is a second, sharper version of the same hazard, documented in the design
notes for a later accelerator: the amd64 and i386 accelerator dispatch used a
*binary ternary* — `syscall_num == 0xacc0 ? aead(...) : pixop(...)` — so adding
a third accelerator number to the bypass condition without touching the ternary
would route it silently into the pixman handler. Plausible errno, no crash, no
log. Structures that are correct for two cases and silently wrong for three are
worth noticing before the third case exists.

## 11.6 Errno translation, and the second-copy rule

Guest errno numbers are Linux's. Host errno numbers are Darwin's. They are not
the same numbers, so every host error crossing into the guest passes through
`err_map`, a switch from host constant to guest constant.

This produces a rule that is easy to state and easy to forget: **searching for a
guest errno constant does not find every place it is returned.** A handler that
does `return errno_map();` returns `_EINTR` without the token `_EINTR` appearing
anywhere near it. When Chapter 12 talks about auditing every path that can
return `EINTR`, this is why the audit takes longer than it should.

The convention that makes this manageable is visible in `kernel/fs.c`:

```c
dword_t sys_getcwd_guest(guest_addr_t buf_addr, dword_t size) {
    return sys_getcwd_common(buf_addr, size);
}
dword_t sys_getcwd(addr_t buf_addr, dword_t size) {
    return sys_getcwd_common(buf_addr, size);
}
```

Two entry points, one body. `sys_getcwd_guest` takes a full-width guest address
and is reached by the native dispatch of Section 11.5; `sys_getcwd` takes the
legacy 32-bit form and is reached through the table. Neither contains logic.
Where a syscall follows this shape, the second copy is a two-line wrapper and
the thing that can drift is only the signature.

Where it does not — where two ABIs have genuinely different bodies because the
structures they pass differ — the drift is real, and this is the rule to hold
on to:

> **A syscall often has a second copy.**

Four guest ABIs share most handlers, but not all. Some syscalls have a per-ABI
body — `sys_getitimer_amd64` sits beside the generic one — because the structure
layouts differ. Some have a 32-bit and a 64-bit form. Some are reached through
two dispatch paths, the legacy table and the native full-width intercept. The
`clone` stack write in Chapter 10 sets four different registers because four
different architectures spell "stack pointer" differently.

A fix applied to the copy you found is a fix missing from the copies you did
not. The mechanical check is to grep for the syscall's *name* rather than its
number, in `kernel/` and `fs/` together, and to read every hit before editing
any of them.

## 11.7 Watching a syscall happen

Two mechanisms make this layer observable, and they are the workhorses of
Chapter 36.

`STRACE(...)` is a macro compiled in when the build enables the `strace` log
channel, and every handler calls it with the syscall's parameters and, on the
way out, its result. This produces the single most useful diagnostic in the
project — a line per syscall naming what was asked and what was answered.

`log_stub_syscall` prints the pid, the command name, the ABI, the number, and —
on amd64 — the instruction pointer and the argument registers, whenever an
unimplemented syscall is reached. Including the *command name* was one of the
fork's earliest changes, in December 2021 (Chapter 3), and it is a good example
of a small change with a large effect on debuggability: "stub syscall 352" tells
you nothing, and "pid 412 (systemd-udevd) stub syscall 352" tells you what to go
and read.

It is also rate-limited, which any log on a syscall path has to be: five lines
per (ABI, syscall number) pair, then one "suppressing further" notice and
silence. A program that probes an absent syscall in a loop would otherwise fill
the log with the same line and push out the one that mattered. There is one
hardcoded exception that always logs — amd64 tasks named `bash` — which is a
leftover from an investigation and, like the trace hooks below, has not been
removed.

There is also a family of much narrower hooks — `amd64_tty_process_trace`,
`dpkg_overflow_syscall_trace`, `amd64_tracked_sigabrt_trace`,
`amd64_tracked_enoent_path_trace`, and a dozen more — called around amd64
dispatch. Each was written for one investigation: a specific program failing in
a specific way, where a general trace produced too much output to read. They are
scaffolding that outlived their investigations, they sit in the hot path behind
their own enable checks, and an honest account of this file has to say that
they are technical debt of a particular kind: individually justified, collectively
unexamined.

## 11.8 What a syscall costs

The path is: leave translated code (Chapter 6 — which means a `jit_enter` exit
and, in the historical case that was fixed, a `sigprocmask` pair), read a
number, range check, table index, read six arguments, possibly marshal them,
call a C function, write a result, and re-enter translated code.

None of that is expensive in itself. What makes syscalls the interesting cost
centre is what the handlers do: take locks that other guest threads want
(Chapter 16's `inodes_lock`), touch SQLite (Chapter 17), or block, which means
parking a host thread and later needing the wake machinery of Chapter 10.

The thread benchmark of Chapter 7 is the clean measurement of the floor —
5,000 `clone`s and joins land within 4.5% across all four guests, because that
time is spent in shared kernel code rather than in translation. When a workload
is syscall-bound, the guest architecture stops mattering and everything in
Part III starts to.

---

*Anchors:* [kernel/calls.c](../../kernel/calls.c),
[kernel/errno.c](../../kernel/errno.c), [kernel/errno.h](../../kernel/errno.h),
[kernel/abi/](../../kernel/abi), [emu/interrupt.h](../../emu/interrupt.h),
[kernel/native_syscall.c](../../kernel/native_syscall.c).

*Story:* `uv venv` dying with "Bad system call" on aarch64 — because the legacy
marshaller's dword-fit check cannot tell a 64-bit pointer from a 32-bit `int`
whose upper half holds whatever the compiler last left in the register.
