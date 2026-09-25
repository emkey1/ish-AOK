# Queued for a future release

Work that was filed as a task chip but not started before the 556 freeze
(2026-09-25). Each entry is the chip's summary and then its full original
prompt, verbatim, so a session can start from exactly what was filed. The
prompts name the files, the evidence and the Linux 6.12 (camd) measurements
as they stood that day, so re-check against `working` before relying on a
line number. Indexed from docs/TODO.md ("Queued for a future release").

## Implement ENTER and 16-bit branch EIP truncation on i386 JIT

While fixing 16-bit PUSH/POP on the i386 JIT, I found ENTER (C8) is SIGILL there, and 0x66 near JMP rel/Jcc/LOOP/JCXZ keep the full target where x86 truncates EIP to 16 bits. This session would measure both on camd and fix them with a test.

<details><summary>Original chip prompt</summary>

```text
In iSH-AOK (/Users/mke/git/ish-AOK, branch `working`; follow the project memory: diff peer worktrees before starting, oracle on camd, positive controls, register tests in THREE places, stage explicit paths, push to origin/working and fast-forward the main checkout), two i386-JIT gaps found 2026-09-25 while fixing 16-bit PUSH/POP (commit "i386: the 0x66 stack instructions move two bytes, and PUSHA and POPA fault as one instruction", test tests/manual/x86/i386_push16.c):

1. ENTER (C8 iw ib) is not decoded in emu/decode.h's one-byte table, so `enter $8,$0` is SIGILL on the i386 JIT (checked on build/alpine-i386-test). Implement it (both operand sizes; 66 C8 pushes BP and moves ESP by 2 per level) in jit/gen.c the way the 16-bit stack ops now are: loads/stores at [esp+off] through gen_stack_access plus one esp_add gadget, so a fault leaves ESP unmoved. Nesting levels > 0 copy frame pointers; measure every level count you support on camd first.

2. With the 0x66 prefix, near JMP rel16 (66 E9), JMP rel8 (66 EB), Jcc rel8/rel16 (66 7x, 66 0F 8x), LOOP/LOOPE/LOOPNE and JCXZ truncate EIP to its low 16 bits when taken (Intel SDM: IF OperandSize = 16 THEN tempEIP AND 0000FFFFH), which on Linux always means SIGSEGV SEGV_MAPERR at an address below 64 KiB. The JIT's JMP_REL/J_REL/JN_REL/JCXZ_REL macros in jit/gen.c use fake_ip + off untruncated (and READIMM of a 16-bit immediate is zero-extended, not sign-extended), so they land inside real code. JMPW r/m16 (66 FF /4), CALLW and RETW are already right. Measure on camd (gcc -m32, SIGSEGV handler reading uc_mcontext EIP/ESP and si_addr, as i386_push16.c does), then fix with the truncated target and add probes to a new i386-only test (name it i386_*).

Verify on build/alpine-i386-test (full guest suite), on camd with a GCC build (x86_64 gadgets: git archive the tree + deps/smallclue to /tmp, meson setup with -Dnative_*=disabled), and confirm the amd64 JIT is untouched.
```

</details>

## Fix x86_fp_env failures on the x86_64-host gadgets

Running the x86 guest tests under a GCC/x86_64-host build on camd, x86_fp_env fails two SSE checks (ucomisd qnan, divsd FTZ), with or without today's changes; aarch64 builds pass. This session would find and fix the x86_64-backend cause.

<details><summary>Original chip prompt</summary>

```text
In iSH-AOK (/Users/mke/git/ish-AOK, branch `working`; follow the project memory: diff peer worktrees first, oracle on camd, stage explicit paths, push to origin/working and fast-forward the main checkout), tests/manual/x86/x86_fp_env.c fails on an ish built for an x86_64 HOST (the jit/gadgets-x86_64 backend), measured 2026-09-25 on camd with a GCC debug build (meson -Dnative_*=disabled) running camd's Alpine 3.11 i686 root (copy of ~/ish-AOK/e2e_out/testfs):

    FAIL ucomisd qnan: got 0x1 expected 0
    FAIL divsd DBL_MIN/3 ftz value: got 0x5555555555555 expected 0
    x86_fp_env: FAIL failures=2

It fails identically on a build without that day's i386 stack changes (/tmp/vg-gcc/build-gcc/ish, c5b34ec2), so it predates them; the same test passes on the aarch64-host build on alpine-i386 and alpine-amd64. Read the guest-fp-environment memory (emu/fpenv.c enter/exit, MXCSR FTZ/DAZ handling) and find why the x86_64 host backend does not apply MXCSR.FTZ and reports an unordered compare wrongly for a QNaN operand. The x86_64 host does not ship (iOS is aarch64), but Linux CI builds it and GCC-on-camd runs are how the project checks it, so fix it or document why it cannot be fixed there. Verify with the camd GCC build and the full x86 guest subset.
```

</details>

## Make meminfo Shmem/AnonPages/Mapped Linux-shaped

While making /proc/meminfo cheap, a side-by-side run against Linux 6.12 showed AOK's Shmem, AnonPages and Mapped mean something different: mapped entries per process from mmap, not resident pages once each. A new session would change the accounting to Linux's meaning and update the test.

<details><summary>Original chip prompt</summary>

```text
In iSH-AOK (/Users/mke/git/ish-AOK, branch `working`; follow the project memory: oracle on camd, positive controls, register tests in THREE places, stage explicit paths, push to origin/working and fast-forward the main checkout), make /proc/meminfo's Shmem, AnonPages and Mapped lines report what Linux reports.

Background: the meminfo session of 2026-09-25 (commit "meminfo: Shmem, AnonPages and Mapped are counters, not a walk of every page") turned these three lines into per-address-space counters (struct mem class_entries[], emu/memory.c mem_entries_published / mem_entry_cleared, summed in fs/proc/root.c collect_mem_page_stats). It deliberately kept the old walk's VALUES, which are not Linux's. docs/TODO.md ("What is still missing from procfs", the /proc/meminfo paragraph) has the side-by-side table measured on amd64 vs Linux 6.12 (camd). In short:
- AOK counts page-table ENTRIES per address space from mmap time; Linux counts RESIDENT pages, once each, from first touch.
- AnonPages (Linux) = anonymous pages mapped anywhere, counted once (a forked child sharing COW pages adds nothing until it writes). AOK adds the child's whole copy.
- Mapped (Linux) = file-backed pages incl. shmem/memfd mapped anywhere, counted once per page (a memfd mapped RW and RX counts once). AOK counts per mapping: a 2 GiB memfd mapped twice shows Mapped +4 GiB untouched; Linux shows 0.
- Shmem (Linux) = every shmem page (memfd, MAP_SHARED|MAP_ANONYMOUS, tmpfs files, SysV shm), mapped or not; it survives munmap while the fd is open. AOK counts only shared-anonymous entries, never memfd.

Likely shape: per-frame state on struct data (it already tracks owners/frame_refs) with global counters moved when a frame's first touched mapping appears/disappears, plus tmpfs/memfd page accounting for Shmem. Keep reads O(1) or O(tasks) -- the whole point of the earlier change was that the .NET GC reads this file on every collection. Keep ISH_MEM_CLASS_CHECK=1 (or an equivalent walk-and-compare) working. Update tests/manual/meminfo_scaling.c so its value legs assert Linux's figures (camd is the oracle: `ssh camd`, gcc and gcc -m32), with a positive control showing the new assertions fail on the old build.
```

</details>

## Raise #GP for privileged and misaligned x86 instructions

While fixing how iSH-AOK reports a #GP, the camd oracle showed several instructions Linux faults with #GP that iSH-AOK either answers with SIGILL or runs silently. This session would decode them and raise #GP(0) on both x86 engines, with tests measured against camd.

<details><summary>Original chip prompt</summary>

```text
In iSH-AOK (/Users/mke/git/ish-AOK, branch `working`; follow the project memory: oracle on camd, positive controls, stage explicit paths, push to origin/working and fast-forward the main checkout), make these x86 instructions raise #GP the way x86_64 Linux 6.12 does, on both the i386 engine (emu/decode.h + jit/gen.c) and the amd64 engines (emu/amd64_interp.c interpreter and its JIT bridges in jit/gen.c).

Measured on camd (Zen+, Linux 6.12) with gcc -m64 and -m32, every one of these is SIGSEGV, si_code SI_KERNEL, si_addr NULL, REG_TRAPNO 13, REG_ERR 0, saved IP at the instruction:
- Privileged 0F opcodes: RDMSR (0f 32), WRMSR (0f 30), MOV from/to CR0 (0f 20 / 0f 22; also DRn 0f 21/23), CLTS (0f 06), INVD (0f 08), WBINVD (0f 09), RDPMC (0f 33, with ECX=0x40000000), LGDT (0f 01 /2). Probably also LIDT, LLDT, LTR, LMSW, INVLPG (check each on camd). iSH-AOK raises SIGILL (ILL_ILLOPN, trapno 6) for all of them today. SGDT/SIDT/SMSW/SLDT/STR depend on UMIP (camd's Zen+ has none, so they run natively there) -- measure and decide what CPUID says.
- Misaligned 16-byte-aligned SSE accesses: MOVAPS load from buffer+8, MOVDQA store to buffer+4 -- iSH-AOK performs them with no fault. Likely the same class: MOVAPD, MOVNTPS/MOVNTDQ, aligned VEX forms (VMOVAPS/VMOVDQA with 16/32-byte alignment), CMPXCHG16B, FXSAVE/FXRSTOR, XSAVE family -- measure each on camd before asserting.
- IRET (CF) is not decoded on the i386 engine at all (SIGILL). On camd -m32, a same-privilege IRET returns normally, and IRETD to CS 0/0x2b/0x13/0x10/0x07 is #GP with REG_ERR = CS & 0xfffc (0, 0x28, 0x10, 0x10, 0x4); CS 0x33 faulted reported 6 bytes later (it entered 64-bit mode). The amd64 engine's amd64_iret_op in emu/amd64_interp.c is the model.

How a #GP with an error code is raised now (commit "x86: a #GP reports no address and its error code..."): return INT_GPF for #GP(0), or INT_GPF_CODE(code) from emu/interrupt.h for a nonzero code; gen.c's PRIV() macro emits #GP(0) at the instruction for the i386 JIT; the kernel's handle_general_protection_interrupt (kernel/calls.c) delivers it. Add probes to tests/manual/x86/gpf_siginfo.c (its header lists these as "Not asserted") or a new test registered in the three places (fs/aok-tests.manifest, need_file and all_tests in tests/manual/setup-regressions.sh). Verify on build/alpine-amd64-test, build/devuan-amd64-test, build/alpine-i386-test and on camd (scp the test with tests/manual/test_common.h; gcc -m64 and -m32 -msse2).
```

</details>

## amd64: int n, sigreturn CS/SS, non-canonical jumps

The #GP oracle work found three amd64 gaps: every `int n` (CD) is SIGILL, rt_sigreturn ignores a broken CS or SS, and a jump to a non-canonical address is reported at the target instead of the jump. This session would fix all three against measured camd behaviour.

<details><summary>Original chip prompt</summary>

```text
In iSH-AOK (/Users/mke/git/ish-AOK, branch `working`; follow the project memory: oracle on camd, positive controls, stage explicit paths, push to origin/working and fast-forward the main checkout), fix three amd64-guest divergences from x86_64 Linux 6.12, all measured on camd (Zen+, gcc -m64):

1. `int imm8` (CD) is not decoded by either amd64 engine (emu/amd64_interp.c and the amd64 JIT in jit/gen.c): every vector is SIGILL. Linux: `int $3` (cd 03) -> SIGTRAP si_code SI_KERNEL, REG_TRAPNO 3, IP after the 2 bytes; `int $4` -> SIGSEGV SI_KERNEL, REG_TRAPNO 4, IP after; `int $0x80` -> a 32-bit syscall through the IA-32 table (eax = number, ebx/ecx/edx/esi/edi/ebp = args truncated to 32 bits, result in eax) -- check what iSH-AOK's i386 syscall table can offer an amd64 task and decide; every other vector -> #GP(vector*8+2): SIGSEGV SI_KERNEL, si_addr NULL, REG_TRAPNO 13, REG_ERR 0x40a for 0x81, 2 for 0, 0xa for 1, 0x2a for 5, 0x7fa for 0xff, IP at the instruction. The i386 engine does this in jit/gen.c's SOFT_INT macro; raise the #GP with INT_GPF_CODE(code) (emu/interrupt.h), INT_OVERFLOW has a kernel case (handle_overflow_interrupt in kernel/calls.c).
2. rt_sigreturn on amd64 (sys_rt_sigreturn_amd64 in kernel/signal.c) ignores the CS and SS words of REG_CSGSFS. Linux takes them (RPL forced to 3) and the IRET faults at the resume IP: SIGSEGV SI_KERNEL, REG_TRAPNO 13, REG_ERR: CS 0x2b -> 0x28, 0x13 -> 0x10, 0x10 -> 0x10, 0 -> 0, 0x07 -> 0x4, 0x0f -> 0xc, 0x4007 -> 0x4004; with CS 0x33: SS 0x23 -> 0x20, 0x13 -> 0x10, 0 -> 0, 0x3b -> 0x38, 0x1b -> 0x18, 0x4003 -> 0x4000, and any SS with the TI bit (0x07, 0x0f, 0x4007) -> 0 (espfix), also with a bad CS. i386's version is i386_sreg_sigreturn_cs_ss in emu/i386_sreg.c. Mind UC_STRICT_RESTORE_SS: without it Linux's force_valid_ss replaces a bad SS instead of faulting -- measure. CS 0x23 would enter 32-bit mode on Linux.
3. A jmp/call/ret to a non-canonical target (0x8000000000001000): camd reports the #GP at the jump (REG_RIP = the jmp); iSH-AOK reports it with RIP = the target (amd64_bad_transfer_target in emu/amd64_interp.c sets amd64_rip = target, and the JIT path faults on fetching the target). Measure ret (and RSP at the fault) on camd before changing it; keep the JIT hot path cheap.

Tests: extend tests/manual/x86/gpf_siginfo.c (its header lists these as "Not asserted") or add a test registered in fs/aok-tests.manifest plus need_file and all_tests in tests/manual/setup-regressions.sh. Verify on build/alpine-amd64-test, build/devuan-amd64-test and camd.
```

</details>

## Match Linux's x86 page-fault REG_ERR and CR2

The #GP oracle runs also showed the page-fault frame's error-code bits differ from Linux (a plain read of a PROT_NONE page claims to be an instruction fetch), and CR2 is not kept across frames. This session would make REG_ERR and REG_CR2 match camd on both x86 ABIs.

<details><summary>Original chip prompt</summary>

```text
In iSH-AOK (/Users/mke/git/ish-AOK, branch `working`; follow the project memory: oracle on camd, positive controls, stage explicit paths, push to origin/working and fast-forward the main checkout), make the x86 page-fault signal frame match x86_64 Linux 6.12 (camd, gcc -m64 and -m32).

signal_trap_error in kernel/signal.c computes REG_ERR for INT_PF: 4 (user) | 2 (write) | 1 if mem_segv_reason is SEGV_ACCERR | 0x11 (instruction fetch + present) for any non-write fault on a page that is not executable. Measured: a read of a PROT_NONE page gives err 0x4 on camd (both ABIs) but 0x15 on iSH-AOK -- the fetch bit is set for a data read, and PROT_NONE is "not present" to the hardware. A read/write of the canonical kernel address 0xffff888000001000 on amd64 gives 0x5/0x7 on camd (Linux's sanitize_error_code sets PROT for addresses >= TASK_SIZE_MAX) but 0x4/0x6 on iSH-AOK. Linux's P bit also depends on whether the page was populated (a write to a never-touched read-only mapping is 6, to a populated one 7) -- measure the cases that matter (PROT_NONE, read-only written before/after being read, unmapped, NX fetch via INT_PF_EXEC) and decide what iSH-AOK can know (it has PT_TOUCHED-style state; see the VM usage counters memory). The fetch bit belongs only to real instruction fetches (handle_exec_fault_interrupt in kernel/calls.c).

Also: Linux keeps the last page fault's address in thread.cr2 and writes it into every later frame, so a #GP after a page fault shows REG_CR2 = that address (measured); iSH-AOK writes CR2 only when trapno == INT_PF (setup_sigcontext and setup_rt_sigframe_amd64). Linux likewise writes thread.trap_nr and error_code into every frame, not only synchronous-signal ones; decide whether to model that.

Test: add probes (a PROT_NONE read and write, a read-only page write, a kernel address on amd64, a PF followed by a #GP for CR2) to tests/manual/x86/gpf_siginfo.c, which today asserts only (err & 6) == 4 for its page-fault cases, or a new registered test (fs/aok-tests.manifest, need_file and all_tests in tests/manual/setup-regressions.sh). Verify on build/alpine-amd64-test, build/devuan-amd64-test, build/alpine-i386-test and camd.
```

</details>

## Add PTRACE_POKEUSER and base checks to ptrace

While adding the amd64 GS base, I found iSH-AOK has no PTRACE_POKEUSER at all (any ABI), and SETREGS accepts any fs_base/gs_base where Linux says EIO. The new session implements POKEUSER for amd64 and i386 and the base validation, oracle-checked on camd.

<details><summary>Original chip prompt</summary>

```text
In iSH-AOK (/Users/mke/git/ish-AOK, branch `working`; follow the project memory: oracle on camd, positive controls, register tests in THREE places, stage explicit paths, push to origin/working and fast-forward the main checkout), implement PTRACE_POKEUSER (request 6) like Linux, and Linux's putreg validation for amd64 SETREGS.

Found 2026-09-25 while adding the amd64 GS base (tests/manual/x86/amd64_gs_base.c, commit on `working` titled "amd64: a GS override adds a base of its own..."): kernel/ptrace.c implements PTRACE_PEEKUSER_ (amd64: offsets into struct user_regs_struct via get_user_regs_amd64, plus u_debugreg 848..911 reading 0; i386: struct user_) but there is no PTRACE_POKEUSER for any ABI -- the request falls to the default and fails. gdb writes debug registers through POKEUSER (hardware watchpoints) and some tools poke single registers.

Linux x86_64 (arch/x86/kernel/ptrace.c): POKEUSER at an offset inside user_regs_struct goes through putreg (the same path as SETREGS, per word): segment selectors via set_segment_reg (16-bit truncation, EIO unless null or RPL 3 -- see set_user_sreg_amd64 in kernel/ptrace.c, which currently silently ignores a bad one instead of EIO), eflags masked by FLAG_MASK, and fs_base / gs_base EIO when >= TASK_SIZE_MAX (0x7ffffffff000 with 4-level paging). Offsets into u_debugreg go to ptrace_set_debugreg (iSH has no hardware breakpoints: decide from the oracle what a write of 0 vs nonzero should return so gdb degrades gracefully). Unaligned or out-of-range offsets are EIO. Also: SETREGS on Linux applies fields in struct order and stops at the first EIO -- check on camd whether a bad fs_base in SETREGS leaves the earlier registers written, and make set_user_regs_amd64 return an error so SETREGS/SETREGSET can report it. The i386 (32-bit) side has its own struct user_ and putreg32 rules; c5b34ec2 already added i386 selector EIO checks to SETREGS -- reuse them.

Oracle-check every expectation on camd first (gcc and gcc -m32), add a regression test (ptrace_pokeuser or similar) with a positive control, register it in all three places, and verify on build/alpine-amd64-test, build/devuan-amd64-test and build/alpine-i386-test under the JIT.
```

</details>
