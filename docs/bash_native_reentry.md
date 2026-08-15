# Native bash: the re-entry problem, and the globals it turns into crashes

Written 2026-08-15. This is the open work item for native bash, and the reason
it is not yet usable as a login shell.

## The problem in one paragraph

A native program is a C function inside the emulator's process, not a process
of its own — so every one of bash's globals keeps the value the PREVIOUS
invocation left in it. Typing `/AOK/native/bash` a second time in one app
session, or opening a second terminal, hands bash the last bash's state. Where
that state owns heap memory the result is `SIGABRT` — "pointer being freed was
not allocated" — which takes the WHOLE APP down, not just the shell. That is
what booted an ssh login before a prompt ever appeared, and it is why fixes
that looked correct in a single `bash -c` kept failing in real use.

bash has a function for exactly this: `shell_reinitialize()` (shell.c ~2008),
described as "reset the world back to a pristine state, as if we had been
exec'ed", and `main()` already calls it when `shell_initialized` is set
(shell.c:461). It resets prompts, flags, variables and functions. It does NOT
reset globals that own heap memory, because in a normal bash re-entry never
happens. **That is the gap. Every fix below belongs in that one function.**

## Two already fixed

- `export_env` (variables.c:176) — `maybe_make_export_env` opens with
  `if (export_env) strvec_flush (export_env)`, and the array's elements are
  BORROWED from the variable table, not owned. Fixed via
  `aok_reset_export_env()`.
- `already_making_children` — not a crash but the cause of the shell never
  waiting for a command; fixed by calling `making_children()` in
  `aok_register_spawned`.

## The rest, from an audit of bash's sources

Each was proposed by one agent reading a subsystem and then adversarially
verified by another against the source; only findings that survived are here.
Prefer the existing bash helper where one is named — reusing bash's own reinit
code is the difference between a fix and a second implementation that has to
stay in agreement with the first.

Where ownership is ambiguous, DROP without freeing. A leak of one object per
bash invocation is far cheaper than a crash, and that is the precedent set by
`export_env`.

| global | severity | defined | reset |
|---|---|---|---|
| `already_making_children` | corruption | deps/bash/jobs.c:228 | stop_making_children (); — the existing helper at jobs.c:430, or plainly already_making_children = 0. |
| `bgpids (struct bgpids)` | corruption | deps/bash/jobs.c:178 | bgp_clear (); — the existing helper at jobs.c:926. Safe in either order: bgp_resize rebuilds pidstat_table whenever bgpids.nalloc == 0 (jobs.c:764-771 |
| `cached_quoted_dollar_at` | corruption | deps/bash/subst.c:217 | Call the existing helper: `invalidate_cached_quoted_dollar_at ()` (subst.c:10923). Freeing is safe here, unlike export_env — the list is a private dee |
| `current_fds_to_close` | crash | deps/bash/execute_cmd.c:320 | current_fds_to_close = (struct fd_bitmap *)NULL;  -- drop, never free: the bitmap is owned by the stack frame that lent it. |
| `currently_executing_command` | crash | deps/bash/execute_cmd.c:273 | currently_executing_command = (COMMAND *)NULL;  -- drop, never free: the tree belongs to reader_loop / parse_and_execute. |
| `dev_fd_list` | corruption | deps/bash/subst.c:6229 (the HAVE_DEV_FD branch; generated/config.h:541 defines HAVE_DEV_FD, so this branch is the one compiled) | Call bash's own helper `clear_fifo_list ()` (subst.c:6244): it zeroes the marks and nfds WITHOUT closing anything, which is precisely what upstream do |
| `exec_redirection_undo_list` | corruption | deps/bash/execute_cmd.c:240 | exec_redirection_undo_list = (REDIRECT *)NULL;  -- drop without freeing. |
| `handling_termsig` | crash | deps/bash/sig.c:490 | handling_termsig = 0; -- plain int, no ownership. Highly reachable: any first bash that takes a SIGPIPE (`yes / head -1`) or a SIGHUP latches it perma |
| `ifs_value` | corruption | deps/bash/subst.c:155 | Same single call as ifs_var: `setifs ((SHELL_VAR *)NULL)` (subst.c:12034). Never free ifs_value — subst.c does not own it, and after a shell where IFS |
| `ifs_var` | corruption | deps/bash/subst.c:154 | `setifs ((SHELL_VAR *)NULL)` (subst.c:12034) — drop, never free; the variable table owns it. That one call also rebuilds ifs_value, ifs_cmap, ifs_firs |
| `interrupt_state` | crash | deps/bash/sig.c:66 | interrupt_state = 0; (the CLRINTERRUPT macro, quit.h:42) |
| `jobs` | corruption | deps/bash/jobs.c:183 | jobs = (JOB **)NULL — but ONLY in lockstep with js (next entry). js.j_jobslots is the sole guard on every jobs[] access, so NULLing jobs while leaving |
| `js (struct jobstats, incl. j_lastmade / j_lastasync)` | corruption | deps/bash/jobs.c:175 | init_job_stats () — the existing helper at jobs.c:373 (js = zerojs) — called together with `jobs = NULL`. Resetting js alone merely leaks the array; r |
| `nfds` | corruption | deps/bash/subst.c:6230 | Covered by `clear_fifo_list ()` (subst.c:6244). If reset by hand, `nfds = 0` must be paired with zeroing dev_fd_list — the two disagreeing is worse th |
| `pidstat_table` | corruption | deps/bash/jobs.c:177 | None on its own — it is rebuilt to NO_PIDSTAT by bgp_resize when bgpids.nalloc == 0 (jobs.c:764-768), which bgp_clear() guarantees. Listed because the |
| `pipeline_pgrp` | corruption | deps/bash/jobs.c:203 | pipeline_pgrp = (pid_t)0; — no memory involved, but a stale value puts bash #2's first child into a process group belonging to a dead (or recycled) gu |
| `procsubs (struct procchain)` | corruption | deps/bash/jobs.c:180 | procsubs.head = procsubs.end = 0; procsubs.nproc = 0; — drop without freeing. bash's own procsub_clear() (jobs.c:1114) is the freeing alternative and  |
| `queue_sigchld` | corruption | deps/bash/jobs.c:318 | queue_sigchld = 0; — bash sets exactly this itself on its longjmp cleanup path, wait_sigint_cleanup (jobs.c:2794-2799), which is the precedent. Left n |
| `redirection_undo_list` | corruption | deps/bash/execute_cmd.c:235 | redirection_undo_list = (REDIRECT *)NULL;  -- drop without freeing, and specifically do NOT call undo_partial_redirects()/dispose_partial_redirects(): |
| `return_catch_flag (with return_catch, the jmp_buf it guards, and return_catch_value)` | crash | deps/bash/execute_cmd.c:218 (flag), :220 (procenv_t return_catch), :219 (value) | return_catch_flag = 0; return_catch_value = 0;  -- exactly what bash's own initialize_subshell does at execute_cmd.c:5955. The jmp_buf itself needs no |
| `sh_coproc` | corruption | deps/bash/execute_cmd.c:1790 | coproc_init (&sh_coproc);  -- bash's own helper at execute_cmd.c:2037-2046. It drops c_name without freeing and sets c_rfd/c_wfd to -1 without closing |
| `sigchld` | corruption | deps/bash/jobs.c:317 | sigchld = 0; — reset with queue_sigchld. A stale positive count makes a fresh bash's first blocking wait spin non-blocking and return before the child |
| `sigterm_received` | crash | deps/bash/sig.c:72 | sigterm_received = 0; |
| `terminating_signal` | crash | deps/bash/sig.c:75 | terminating_signal = 0; |
| `the_pipeline` | corruption | deps/bash/jobs.c:219 | the_pipeline = (PROCESS *)NULL; — drop, do not discard. Left non-NULL when bash #1 exits between make_child and stop_pipeline. Note the interaction wi |
| `the_printed_command_except_trap` | corruption | deps/bash/execute_cmd.c:215 | the_printed_command_except_trap = (char *)NULL;  -- drop without freeing (one leaked string per invocation). NULL is a value bash already assigns itse |
| `unwind_protect_list` | crash | deps/bash/unwind_prot.c:90 | clear_unwind_protect_list (0); -- the flags==0 path in clear_unwind_protects_internal (unwind_prot.c:253-262) sets the head to NULL WITHOUT freeing, w |
| `wait_signal_received` | crash | deps/bash/trap.c:120 | wait_signal_received = 0; -- and it only bites in company, so the other agent must also zero wait_intr_flag (builtins/wait.def:91) at the same point;  |

## How to know when it is done

**bash's own test suite is the oracle.** It ships 80 `.tests` files with
expected output. It is already unpacked at `/bt/tests` in
`/Users/mke/pscal-ish-stage0/roots/pscal-devuan-arm64`, and runs with:

    ./build/ish -f <root> /bin/sh -c \
      'cd /bt/tests && THIS_SH=/AOK/native/bash /AOK/native/bash alias.tests'

Report a pass count out of 80. Do NOT report progress from hand-typed commands:
every failure in this class needs a SECOND bash invocation to appear, and
single commands cannot reach it. That mistake cost five rebuild cycles.

At the time of writing the suite aborts on the first file.

## Also still open

- One `make_child` site is unconverted — `./alias1.sub: fork: Function not
  implemented`. Candidates are process substitution (subst.c:6536), the
  coprocess (execute_cmd.c:2421), and the null command in a pipeline
  (execute_cmd.c:4107).
