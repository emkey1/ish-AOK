# build 553 musts

Work deliberately deferred out of 552, with the diagnosis already done so
nobody has to re-derive it. Each entry says what is **established**, what the
**next step** is, and how to **prove** it afterwards.

Started 2026-09-01, during the 552 release run.

---

## amd64 never JIT-compiles a locked instruction

**Established.** Every eligibility predicate in the amd64 JIT front-end
requires the lock prefix to be absent (`jit/gen.c`, `amd64_jit_plain_prefixes`
and its four siblings):

```c
static bool amd64_jit_plain_prefixes(const struct amd64_jit_insn *insn) {
    return !insn->operand_size_prefix &&
        !insn->address_size_prefix &&
        !insn->fs_prefix &&
        !insn->lock_prefix &&                 /* <-- */
        insn->rep_mode == amd64_jit_rep_none;
}
```

So a `lock`-prefixed instruction on an amd64 guest always falls out of the JIT
and is interpreted by `emu/amd64_interp.c`, which implements locked
`xadd`/`cmpxchg`/`cmpxchg16b` as a plain read-compute-write wrapped in
`atomic_l_lock` — a **global** software lock shared by the whole emulator.

The i386 JIT does the opposite: it compiles atomics into gadgets that use real
host atomics — `ldaxr`/`stlxr` LL/SC loops in `jit/gadgets-aarch64/math.S` and
`bits.S`.

That leaves two implementations of guest atomicity that do not interlock with
each other:

| path | mechanism | interlocks with a host atomic? |
| --- | --- | --- |
| i386 JIT gadgets | `ldaxr`/`stlxr` on the guest word | yes |
| amd64 interpreter | global `atomic_l_lock` | **no** |

**Why it matters, twice over.**

*Correctness.* Kernel code that does a read-modify-write on guest memory with
a host atomic does not serialise against an amd64 guest's own atomics. That is
not hypothetical: `FUTEX_WAKE_OP` lost 1107 of 40000 increments with one guest
thread doing atomic adds while another drove WAKE_OP, and its comment
explicitly (and wrongly, for amd64) assumed "the emulator implements guest
atomics with host atomics on this same memory". Fixed for 552 in
`kernel/futex.c` by having the kernel take `atomic_l_lock` too — correct, but
it is agreement with the weaker mechanism rather than a repair of it. Any
future kernel-side RMW on guest memory inherits the same trap, and nothing in
the code stops someone writing one.

*Throughput.* Every `lock`-prefixed instruction an amd64 guest executes pays
twice: it drops out of the JIT into the interpreter, and it serialises the
entire emulator on one global lock. Locked instructions are not rare — they
are every uncontended mutex acquire, every refcount, every `std::atomic`, and
all of glibc's condition-variable machinery.

**Next step.** Give the amd64 JIT the locked forms, emitting the same
host-atomic gadgets the i386 path already uses. `xadd`, `cmpxchg` and the
locked ALU group first — those are what real code emits. Once they are JIT'd
with host atomics, `atomic_l_lock` can come off the amd64 interpreter path,
and `kernel/futex.c`'s WAKE_OP can drop back to a plain host CAS (remove the
lock, keep the atomics, keep the comment explaining why).

Watch for what pushed the interpreter to a global lock in the first place:
unaligned atomics, and ones straddling a page boundary. `jit/helpers.c`'s
`helper_atomic_cmpxchg8b` keeps `atomic_l_lock` for exactly that case and says
so — glibc and Python land 64-bit atomics on 4-aligned addresses routinely. A
JIT fast path for the aligned case with a lock-guarded slow path for the rest
is the shape to aim for, not a wholesale replacement.

**Prove it.** `concurrent_dir_futex` is the correctness witness: it fails on
amd64 with the futex-side lock removed and passes with real interlocking. The
x86 atomics tests (`atomic_xadd32`, `atomic_cmpxchg32`, `atomic_cmpxchg8b`,
`atomic_logic32`, `cow_atomic_fault`) guard guest-against-guest and must stay
green — note they passed throughout the bug above, so they are necessary and
**not sufficient**: they cannot see a guest-against-host race at all. Something
covering that gap directly is worth adding with the fix. For throughput, an
uncontended mutex loop on amd64 against the same on i386 is the honest
before/after.

---

## RLIMIT_STACK is not pushed down for a third party

**Established.** `mem_growsdown_allowed` bounds stack growth by a copy of
RLIMIT_STACK cached in `struct mem`, because the fault path holds `mem->lock`
and `rlimit_get` takes `group->lock`, which better than a hundred sites take
before touching guest memory — reading it live would invert that nesting.

`rlimit_set` pushes changes down only for the calling task. `prlimit64`
against **another** process updates that process's limits without updating its
address space, so the new limit takes effect at its next exec rather than
immediately. Deliberate: reading another task's `->mm` there needs
`general_lock`, and the stack stays bounded by the guard gap meanwhile, so the
failure mode is "bounded less tightly than asked", never unbounded.

**Next step.** Only worth doing if something real uses `prlimit64` to lower
another process's stack limit and expects it to bite immediately. Take
`general_lock` around the `task->mm` read, or hand the update to the target
task to apply at its next syscall boundary.

**Prove it.** A test that lowers another process's RLIMIT_STACK with
`prlimit64` while it is running, then makes it recurse, and requires the fault
at the new limit rather than the old one.
