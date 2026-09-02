iSH-AOK 553

22 commits. A short cycle, and almost all of it went on two things that were
quietly wasting a great deal of CPU.

An idle poll() burned a whole core. Any process waiting in poll() or select()
on a quiet socket spun a host CPU at ~97% for the entire wait. On a device that
meant an idle chronyd, an idle rsyslogd and every sshd-session pinning a core
apiece -- all reported as sleeping, and issuing no syscalls at all while they
did it, because the spin was inside the emulator rather than in guest code. The
cause was Darwin's kqueue write filter, which is registered for hangup
detection even when the guest only wants to read: it is level-triggered, a
quiet socket is always writable, so the wait woke immediately, found nothing
the guest had asked for, and slept again, forever. A guard meant to prevent
exactly this had been there for a long time; Darwin does not honour it on these
objects. Measured before and after on the same binary, the three socket kinds
that were seen spinning went from 95-97% CPU to 0%, with readiness and hangup
still waking the poll in 0.20s. Nothing in the test suite could see it, because
the answer poll() returned was always correct -- only what it cost to produce
that answer was wrong.

Locked instructions on an amd64 guest were not atomic. Not "atomic against the
kernel but not the host", which is what the previous cycle had written down as
a known gap -- genuinely not atomic, against other guest threads, in the
ordinary `lock addl %reg, (mem)` case: four threads incrementing a shared
counter lost 3876 of 200000 increments. The same audit found that `xchg` on
memory, which is atomic on x86 whether or not you write the prefix and is the
store half of every spinlock, did not merely weaken under contention but
livelocked -- two threads on a one-word lock managed 78 acquisitions and then
one spun two hundred million times without ever seeing it released. Three more
forms turned out to have a second implementation in the interpreter that never
locked at all, and on i386 `lock adc`/`lock sbb` read the carry flag inside
their own retry loop, which the arithmetic overwrites, so a contended retry
computed a different instruction. Every locked form the amd64 guest implements
now runs as a real host atomic when the operand is naturally aligned. A new
test runs nineteen of them from four threads at once, because a lost update is
the only symptom a broken atomic has and no single-threaded test can produce
one -- every existing atomics test passed throughout all of the above.

Smaller things. ktop's %CPU column was multiplied by the refresh delay, so a
process using one core read as 301%. getrusage's ru_maxrss could latch a
garbage sample and then hand it to every descendant, reporting 2.7 TB for a
process whose real peak was 4 MB. `lock xchg` was missing from the i386 opcode
table entirely and killed the guest with SIGILL. The whole book now ships on
the device at /AOK/docs/book, and /AOK/docs finally has an index -- it had 21
documents and linked six of them. /AOK is a shortcut in the File Manager
sidebar. Compiled tests are cached in /AOK/fakefs, which should turn most of a
release gate from hours into minutes.

The release gate itself had a bug worth naming: it tested the exit status of a
pipeline's last command rather than the test run's, so a failed end-to-end leg
never counted and it could print GATE CLEAN with a red leg six lines above.

Validated: full guest suite on all four architectures plus a glibc root --
arm64 185, i386 190, amd64 191, riscv64 179, devuan-arm64 185, zero failures --
the end-to-end suite 7/7, and an M4 iPad Pro running the full suite, 186 passed
and none failed. Two device failures were chased rather than waved through: one
was the ru_maxrss bug above, and one passed 3/3 standalone and on every local
leg, recorded rather than dismissed. Every fix was checked against a real Devuan
6 / Linux 6.12 machine before being called a fix.

Known: the page-table walk behind ru_maxrss is still lock-free and can read a
leaf array another thread is freeing -- what shipped bounds the damage rather
than removing the race; `lock not` and `lock neg` still raise SIGILL on an i386
guest; poll() reports a closed socket as hung up but not readable, where Linux
reports both; strace and gdb still kill a thread they attach to; and the amd64
JIT still bridges locked instructions to a helper instead of compiling them,
which costs throughput but no longer costs correctness.
