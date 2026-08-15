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

Fifty-nine confirmed entries follow, crash-severity first. They are given in
full rather than as a table because the *reset* is where the danger is: several
are only safe in combination, and an abbreviated version of this list is worse
than none. Two examples of why —

- `jobs` must be reset **in lockstep with `js`**. `js.j_jobslots` is the sole
  guard on every `jobs[]` access, so NULLing `jobs` while leaving
  `js.j_jobslots > 0` turns jobs.c:588 into a NULL dereference. Resetting `js`
  alone merely leaks the array; resetting `jobs` alone crashes.
- For `sh_coproc` use `coproc_init()`, NOT `coproc_flush()` or
  `coproc_dispose()`. The first drops the name and sets the descriptors to -1;
  the latter two free the name and *close descriptors the fresh shell now owns*.

### `current_fds_to_close` — crash

*Defined* deps/bash/execute_cmd.c:320

*Read before assignment* deps/bash/subst.c:6692 -- `if (current_fds_to_close) { close_fd_bitmap (current_fds_to_close); ... }` in the process-substitution child path. close_fd_bitmap (execute_cmd.c:356-370) reads fdbp->size and fdbp->bitmap[i] out of freed storage and calls close(i) for every non-zero byte, so it closes whatever descriptors the garbage bitmap names. This is the second instance already reported.

*Reset* current_fds_to_close = (struct fd_bitmap *)NULL; -- drop, never free: the bitmap is owned by the stack frame that lent it.

### `currently_executing_command` — crash

*Defined* deps/bash/execute_cmd.c:273

*Read before assignment* deps/bash/execute_cmd.c:376-393, executing_line_number(): it dereferences `currently_executing_command->type`, `->value.Cond->line`, `->value.Arith->line`, `->value.ArithFor->line`. Callers that fire long before :608 assigns in a fresh bash: error.c:86 (report_error), builtins/common.c:96 (builtin_error_prolog, the "line %d: " prefix on every builtin error), variables.c:1484 ($LINENO), shell.c:1636, builtins/evalfile.c:237. The guard is `executing && showing_function_line == 0 && (variable_context == 0 || interactive_shell == 0) && currently_executing_command`, and `executing` is set to 1 at shell.c:762 (the -c path) and eval.c:168 before any of those run -- so a stale non-NULL pointer is dereferenced. execute_cmd.c:6133 additionally calls dispose_command() on it in shell_execve's ENOEXEC re-exec path.

*Reset* currently_executing_command = (COMMAND *)NULL; -- drop, never free: the tree belongs to reader_loop / parse_and_execute.

### `handling_termsig` — crash

*Defined* deps/bash/sig.c:490

*Read before assignment* termsig_sighandler, sig.c:543: `if (handling_termsig) kill_shell (sig);` -- read before anything in a fresh bash could assign it. kill_shell (sig.c:628) then does set_signal_handler(sig, SIG_DFL), kill(getpid(), sig) and exit(128+sig). termsig_handler also short-circuits on it at sig.c:581, so the exit trap, history save and job cleanup are all skipped.

*Reset* handling_termsig = 0; -- plain int, no ownership. Highly reachable: any first bash that takes a SIGPIPE (`yes | head -1`) or a SIGHUP latches it permanently, and every later bash in the app then dies on its first terminating signal instead of handling it.

### `interrupt_state` — crash

*Defined* deps/bash/sig.c:66

*Read before assignment* QUIT, quit.h:38: `if (interrupt_state) throw_to_top_level ();` -- the first QUIT in a fresh bash. throw_to_top_level (sig.c:401) then calls run_interrupt_trap on the previous bash's trap state (sig.c:424), run_unwind_protects on the previous bash's list (sig.c:455), and jump_to_top_level. This is the most likely trigger for the unwind_protect_list crash above, so the two must be reset together.

*Reset* interrupt_state = 0; (the CLRINTERRUPT macro, quit.h:42)

### `ps0_prompt / ps1_prompt / ps2_prompt (and current_prompt_string)` — crash

*Defined* deps/bash/parse.y:238, :241, :246 (compiled as deps/bash/y.tab.c:288, :291, :296)

*Read before assignment* deps/bash/eval.c:153-157 - `if (interactive && ps0_prompt) ps0_string = decode_prompt_string (ps0_prompt);` dereferences it. Today the deref is not reached with a stale value: yylex calls prompt_again (parse.y:2898-2899) before yyparse returns a command, refreshing all three. In the dangling window between shell_reinitialize and that first prompt_again, only pointer COMPARISONS run (parse.y:2877 prompt_is_ps1, parse.y:5623 get_current_prompt_level, bashline.c:1620/1667). So this is a latent use-after-free, one reordering away from being the next export_env.

*Reset* Drop, never free (borrowed): ps0_prompt = ps1_prompt = ps2_prompt = current_prompt_string = NULL; prompt_string_pointer = NULL. prompt_again refills all of them on the first prompt.

### `return_catch_flag (with return_catch, the jmp_buf it guards, and return_catch_value)` — crash

*Defined* deps/bash/execute_cmd.c:218 (flag), :220 (procenv_t return_catch), :219 (value)

*Read before assignment* deps/bash/builtins/return.def:64-65 -- `if (return_catch_flag) sh_longjmp (return_catch, 1);` with no prior assignment. A bare `return` at the top level of a fresh bash longjmps into a stack frame that no longer exists instead of printing "can only `return' from a function or sourced script". Same unguarded read at trap.c:1004-1005, trap.c:1228-1231, execute_cmd.c:5402.

*Reset* return_catch_flag = 0; return_catch_value = 0; -- exactly what bash's own initialize_subshell does at execute_cmd.c:5955. The jmp_buf itself needs nothing once the flag is zero.

### `shell_name` — crash

*Defined* deps/bash/shell.c:180

*Read before assignment* deps/bash/shell.c:455-459 - `if (shell_initialized || shell_name) { if (*shell_name == '-') shell_name++; ... }`. The stale pointer is dereferenced AND incremented before set_shell_name ever runs, and before shell_reinitialize() is called at line 461. Note the ordering: this is one of the few hazards that cannot be fixed from inside shell_reinitialize.

*Reset* Set shell_name = NULL before bash_main_entry() in kernel/bash_glue.c:44 (or guard the deref: `if (shell_name && *shell_name == '-')`). Careful: shell_name != 0 is the SECOND trigger for calling shell_reinitialize at all, so if it is nulled, shell_initialized must stay set for the reinit to still happen (it is -- shell.c:822 sets it and nothing clears it).

### `sigterm_received` — crash

*Defined* deps/bash/sig.c:72

*Read before assignment* CHECK_SIGTERM, quit.h:73: `if (sigterm_received) termsig_handler (SIGTERM);` -- read before assignment in a fresh bash, and termsig_handler ends in kill_shell -> exit(128+SIGTERM), killing the app. Also read by bashline.c:4796.

*Reset* sigterm_received = 0;

### `stream_list` — crash

*Defined* deps/bash/parse.y:1759 (compiled as deps/bash/y.tab.c:4074)

*Read before assignment* deps/bash/parse.y:1596 - with_input_from_stdin() reads it via stream_on_stack(st_stdin) (parse.y:1835-1843) BEFORE anything assigns it. shell_initialize() has just called initialize_bash_input() (shell.c:1987 -> parse.y:1439), which sets bash_input.getter = NULL. If a surviving saver has type st_stdin, the `if` at 1596 is false, init_yy_io is skipped, the getter stays NULL, and the first yy_getc() (parse.y:1477) calls a NULL function pointer. That is a hard crash of the app on the second bash, with no bad free to blame.

*Reset* Drop without freeing: stream_list = (STREAM_SAVER *)NULL. Do NOT walk it calling pop_stream() -- pop_stream would re-install a dead BASH_INPUT (dangling .name, an fd belonging to the exited task) as the current input and hand saver->bstream back to input.c's buffers[]. Leaking one saver per abnormal exit is the cheap side.

### `terminating_signal` — crash

*Defined* deps/bash/sig.c:75

*Read before assignment* The QUIT macro itself, quit.h:37: `if (terminating_signal) termsig_handler (terminating_signal);` (also CHECK_TERMSIG at quit.h:55, and LASTSIG at quit.h:59). This fires on the first QUIT a fresh bash executes, long before anything assigns it. termsig_handler (sig.c:575) then runs run_exit_trap on the PREVIOUS bash's trap_list[EXIT_TRAP] and falls into kill_shell -> exit(128+sig), taking the app down.

*Reset* terminating_signal = 0;

### `unwind_protect_list` — crash

*Defined* deps/bash/unwind_prot.c:90

*Read before assignment* run_unwind_protects (unwind_prot.c:167) -> unwind_frame_run_internal (unwind_prot.c:300), which calls (*(elt->head.cleanup))(elt->arg.v) at line 326 and restore_variable at line 324. A fresh bash reaches this before pushing any frame of its own: throw_to_top_level calls it at sig.c:455 and top_level_cleanup at sig.c:394, both of which the very first QUIT can trigger (quit.h:38) if interrupt_state is stale. discard_unwind_frame/run_unwind_frame (unwind_prot.c:131,140) also walk and uwpfree every node they pass while hunting for a tag that is not there.

*Reset* clear_unwind_protect_list (0); -- the flags==0 path in clear_unwind_protects_internal (unwind_prot.c:253-262) sets the head to NULL WITHOUT freeing, which is exactly the aok_reset_export_env precedent. Call it from shell_reinitialize. Do not pass 1: uwpfree would push the previous bash's nodes into the ocache for reuse, and the oversized ones from unwind_protect_mem_internal are fine but the borrowed-arg problem is not what freeing fixes.

### `variable_context` — crash

*Defined* deps/bash/variables.c:138

*Read before assignment* deps/bash/variables.c:2746 (make_local_variable's set_local_var_flags label), reached from builtins/declare.def:633. Also variables.c:5243 (new_var_context stamps vc->scope from it) and 2748 (every new variable's context).

*Reset* variable_context = 0; -- shell_reinitialize calls delete_all_contexts (variables.c:5462), which resets shell_variables but NOT variable_context; reset_local_contexts (5473) is the function that does zero it and is never called on this path. A bash that exits from inside a function (f() { exit 0; }; f) leaves it at 1. In the next bash, builtins/declare.def:133 and :593 then accept `local'/`declare' at TOP LEVEL, which calls make_local_variable; the funcenv search at variables.c:2653 and 2666 finds nothing because the only contexts are global (flags 0) and VC_BLTNENV. The 2666 loop is guarded (internal_error at 2671), but the was_tmpvar path at 2645 does `goto set_local_var_flags' with vc == 0 and dereferences it at 2746. `VAR=x declare VAR' at top level satisfies all three conditions: assign_in_env (variables.c:3687) stamps the tempvar's context with the stale value, push_scope adopts the table as VC_BLTNENV (execute_cmd.c:4941) and NULLs temporary_env (4946) so `last_table_searched != temporary_env' holds.

### `wait_signal_received` — crash

*Defined* deps/bash/trap.c:120

*Read before assignment* CHECK_WAIT_INTR, quit.h:62-63: `if (wait_intr_flag && wait_signal_received && this_shell_builtin == wait_builtin) sh_longjmp (wait_intr_buf, 1);`. wait_intr_buf (builtins/wait.def:90) is a jmp_buf armed by setjmp_sigs at wait.def:179 inside the PREVIOUS invocation's wait_builtin frame -- a stack frame that no longer exists. Longjmping into it is unrecoverable. Also read as the exit signal at wait.def:183.

*Reset* wait_signal_received = 0; -- and it only bites in company, so the other agent must also zero wait_intr_flag (builtins/wait.def:91) at the same point; either one alone closes the hole, both is cheap.

### `EOF_Reached` — corruption

*Defined* deps/bash/parse.y:1395 (compiled as deps/bash/y.tab.c:3710)

*Read before assignment* deps/bash/eval.c:71 - reader_loop's `while (EOF_Reached == 0)`, read before any assignment. The second bash falls straight through to `return last_command_exit_value` (eval.c:193) and exits without reading, parsing, or executing one command -- and with an exit status inherited from the previous invocation, since last_command_exit_value is only zeroed by shell_reinitialize.

*Reset* EOF_Reached = 0.

### `aliases` — corruption

*Defined* alias.c:69

*Read before assignment* alias.c:89 (hash_search in find_alias), via alias_expand_word / the parser's alias machinery on the fresh shell's first command

*Reset* aliases = (HASH_TABLE *)NULL; — DROP, do not free. delete_all_aliases would be the tidy call, but free_alias_data (alias.c:168-169) calls clear_string_list_expander for any entry still flagged AL_BEINGEXPANDED, and that walks parse.y's pushed_string_list (y.tab.c:4374) — a stale global from the previous invocation. A dropped table leaks a few hundred bytes; the free path can walk a dangling STRING_SAVER chain. Also set_itemlist_dirty (&it_aliases) so completion stops offering the dropped names.

### `already_making_children` — corruption

*Defined* deps/bash/jobs.c:228

*Read before assignment* deps/bash/jobs.c:423 — making_children returns early when it is set, so start_pipeline() never runs and the stale the_pipeline is never cleaned; also execute_cmd.c:901 gates the whole wait_for on it (the AOK comment at jobs.c:2129-2133 documents exactly this dependency). shell_reinitialize resets running_in_background but not this (shell.c:2024).

*Reset* stop_making_children (); — the existing helper at jobs.c:430, or plainly already_making_children = 0.

### `bgpids (struct bgpids)` — corruption

*Defined* deps/bash/jobs.c:178

*Read before assignment* deps/bash/jobs.c:2666 — wait_for_single_pid calls bgp_search (jobs.c:944) and returns the cached status straight to the caller; make_child (jobs.c:2405) and aok_register_spawned (jobs.c:2153) call bgp_delete (jobs.c:891) first.

*Reset* bgp_clear (); — the existing helper at jobs.c:926. Safe in either order: bgp_resize rebuilds pidstat_table whenever bgpids.nalloc == 0 (jobs.c:764-771). Without it, `wait $pid` in a fresh bash can return the exit status of a PREVIOUS bash's child, because AOK recycles guest pids across native invocations and this is a pid->status cache with no generation counter.

### `buffers / nbuffers` — corruption

*Defined* deps/bash/input.c:160-161

*Read before assignment* deps/bash/input.c:351-377 duplicate_buffered_stream and input.c:467-469 close_buffered_fd both index buffers[fd] for whatever fd the NEW bash is redirecting -- a dead entry is adopted (its b_buffer, still holding the previous shell's script text, is then read out at input.c:526 as this shell's input) or freed and its b_fd closed. input.c:195 make_buffered_stream overwrites an occupied slot with no free at all, so each invocation also leaks a stream plus its buffer.

*Reset* Drop every slot but keep the array: `for (i = 0; i < nbuffers; i++) buffers[i] = NULL;`. Do not null `buffers` itself -- free_buffered_stream (input.c:436) writes buffers[n] unconditionally and would fault. Freeing the entries instead is tempting but ambiguous: B_SHAREDBUF entries alias another stream's b_buffer (input.c:354-361), which is where a double free would come from.

### `cached_quoted_dollar_at` — corruption

*Defined* deps/bash/subst.c:217

*Read before assignment* deps/bash/subst.c:11026-11027 — expand_word_internal's "$@" fast path: `if (cached_quoted_dollar_at) return (copy_word_list (cached_quoted_dollar_at));`. A fresh bash hands back the PREVIOUS invocation's positional parameters. The only invalidators are builtins/common.c:422 (remember_args), builtins/shift.def:87, builtins/source.def:108 and variables.c:5708 (pop_dollar_vars) — none of which runs in a bash started with no positional args, because shell.c:1496 gates shell_add_positional_args on `if (args)`.

*Reset* Call the existing helper: `invalidate_cached_quoted_dollar_at ()` (subst.c:10923). Freeing is safe here, unlike export_env — the list is a private deep copy, not borrowed from the variable table.

### `current_readline_line / current_readline_line_index / current_readline_prompt` — corruption

*Defined* deps/bash/parse.y:1515-1517 (compiled as deps/bash/y.tab.c:3830-3832)

*Read before assignment* deps/bash/parse.y:1526 - yy_readline_get's `if (current_readline_line == 0)` is read before assignment; a surviving line is resumed at current_readline_line_index (parse.y:1577) and readline is never called. In the usual case the dead shell left the index on the terminating NUL, so parse.y:1569 frees it and recurses -- self-healing by luck. It is not luck when the index is mid-line, which readline itself makes possible: the comment at parse.y:1604-1607 says a user can embed a newline in the middle of a collected line. The stale prompt is also used verbatim at parse.y:1543.

*Reset* Drop without freeing: current_readline_line = NULL; current_readline_line_index = 0; current_readline_prompt = NULL.

### `dev_fd_list` — corruption

*Defined* deps/bash/subst.c:6229 (the HAVE_DEV_FD branch; generated/config.h:541 defines HAVE_DEV_FD, so this branch is the one compiled)

*Read before assignment* deps/bash/eval.c:78 — `unlink_fifo_list()` at the top of reader_loop, before the fresh bash executes anything -> subst.c:6333-6334 -> subst.c:6317-6319 `close (fd)` on descriptors this task never opened. Also execute_cmd.c:428 after every top-level command, execute_cmd.c:1129/5547 close_new_fifos -> subst.c:6368-6369, and jobs.c:3954 `find_procsub_child (pid)` -> subst.c:6381-6383, which matches a newly reaped child's pid against the stale pid table and then subst.c:6395 marks that fd for closing.

*Reset* Call bash's own helper `clear_fifo_list ()` (subst.c:6244): it zeroes the marks and nfds WITHOUT closing anything, which is precisely what upstream does on its other re-entry into main (execute_cmd.c:6143, `clear_fifo_list (); /* pipe fds are what they are now */`). Keep the array and totfds — it holds only scalars, is owned here, and is already the right size. Do not free it and do not close anything.

### `dollar_vars` — corruption

*Defined* deps/bash/variables.c:159

*Read before assignment* deps/bash/builtins/common.c:465 (number_of_args, i.e. $#) and every $1..$9 expansion. Nothing assigns them first: shell.c:1478 bind_args builds `args' from argv[arg_start..arg_end] and its whole body is inside `if (args)' at shell.c:1496, so a shell invoked with no operands (plain `bash', or `bash -c CMD' with no extra words) never calls remember_args at all.

*Reset* clear_dollar_vars (); -- the existing helper at variables.c:5633 does exactly `what remember_args (xxx, 1) would have done': frees dollar_vars[1..9], disposes rest_of_args, zeroes posparam_count. Ownership here is unambiguous (always savestring, always freed by remember_args), so a real free is correct rather than a drop. Do NOT touch dollar_vars[0]: shell.c:1503 set_shell_name does FREE(dollar_vars[0]) itself, and it runs at shell.c:467 right after shell_reinitialize, so [0] is already handled correctly.

### `dstack.delimiter_depth / temp_dstack.delimiter_depth` — corruption

*Defined* deps/bash/parse.y:2289 and :2294 (compiled as deps/bash/y.tab.c:4604, :4609)

*Read before assignment* deps/bash/parse.y:2491-2496 and :2526 - current_delimiter(dstack) in shell_getc. A stale depth makes the new shell behave as though it is inside an unterminated quote: history expansion is suppressed, blank lines are pushed into history, prompt_again picks PS2, and parse_matched_pair takes the quoted path.

*Reset* dstack.delimiter_depth = 0; temp_dstack.delimiter_depth = 0. Leave both .delimiters buffers and .delimiter_space alone -- they are matched pairs and reusing them is correct.

### `eol_ungetc_lookahead` — corruption

*Defined* deps/bash/parse.y:2322 (compiled as deps/bash/y.tab.c:4637)

*Read before assignment* deps/bash/parse.y:2343-2348 - the first thing shell_getc does is `if (eol_ungetc_lookahead) { c = eol_ungetc_lookahead; ...; return c; }`, before any assignment. A character left over from the dead shell is injected ahead of the new shell's first token.

*Reset* eol_ungetc_lookahead = 0.

### `exec_redirection_undo_list` — corruption

*Defined* deps/bash/execute_cmd.c:240

*Read before assignment* deps/bash/redir.c:256-257 -- `if (exec_redirection_undo_list) dispose_exec_redirects ();` inside do_redirections(RX_UNDOABLE), which lands in execute_cmd.c:498-506 and frees the previous shell's nodes before this shell ever assigned any. Also freed unconditionally at execute_cmd.c:5492 for any non-exec builtin with a redirection. Today those nodes are leaked-but-live so the free is legal; it stops being legal the moment two native bash invocations overlap in the shared address space, at which point the fresh shell frees the parent's live list.

*Reset* exec_redirection_undo_list = (REDIRECT *)NULL; -- drop without freeing.

### `execignore` — corruption

*Defined* findcmd.c:82

*Read before assignment* findcmd.c:104 — `for (p = execignore.ignores; p && p->val; p++)` in exec_name_should_ignore, called from file_status (findcmd.c:137) during the fresh shell's first $PATH search

*Reset* execignore.ignores = 0; execignore.num_ignores = 0; execignore.last_ignoreval = 0; — all three together, via a small helper in findcmd.c since the struct is static. Clearing only last_ignoreval is NOT enough: setup_ignore_patterns returns early at pathexp.c:588 when both this_ignoreval and last_ignoreval are NULL, leaving the previous shell's patterns armed. Dropping leaks a handful of strings; freeing them first is also safe here (split_ignorespec and savestring both allocate fresh), but drop matches the export_env precedent.

### `fignore` — corruption

*Defined* bashline.c:2974

*Read before assignment* bashline.c:3110 — `if (fignore.num_ignores == 0)` in filename_completion_ignore, then bashline.c:3081 in name_is_acceptable, which walks fignore.ignores with NO null check

*Reset* fignore.ignores = 0; fignore.num_ignores = 0; fignore.last_ignoreval = 0; — and it must be all three atomically. bashline.c:3081 dereferences .ignores unguarded and is reached only because bashline.c:3110 trusted num_ignores; clearing num_ignores while leaving ignores (or the reverse) turns a stale-state bug into a NULL dereference. Same pathexp.c:588 early-return trap as execignore.

### `get_groupset.group_set` — corruption

*Defined* deps/bash/variables.c:1686 (function-local static in get_groupset)

*Read before assignment* deps/bash/variables.c:1688 `if (group_set == 0)'. initialize_dynamic_variables recreates GROUPS as a fresh EMPTY array every invocation (variables.c:1943, init_dynamic_array_var), but the stale non-NULL group_set makes get_groupset skip the fill -- so ${GROUPS[@]} is empty in every bash after the first. Reproducible, no crash.

*Reset* group_set = (char **)NULL; -- drop only, never free: general.c's group_vector owns that storage and hands back the same pointer forever. Needs the static lifted to file scope (or a small AOK setter beside get_groupset) to be reachable from the reset helper.

### `group_array / ngroups / maxgroups` — corruption

*Defined* general.c:1237 (group_array), general.c:1234 (ngroups, maxgroups)

*Read before assignment* general.c:1313 — `if (ngroups == 0) initialize_group_array ();` in group_member: a non-zero stale ngroups means the fresh bash never calls getgroups again and answers group membership from the previous invocation's credentials

*Reset* ngroups = 0; group_array = 0; (drop — one small array per invocation) plus the two derived caches below. This one is AOK-specific and real: a native bash is a function call in one process, so the uid/gid the second invocation runs under can differ from the first (su in the guest), and group_member feeds the permission logic in test.c and sh_eaccess.

### `group_vector / group_iarray` — corruption

*Defined* general.c:1333 (group_vector), general.c:1367 (group_iarray)

*Read before assignment* general.c:1336 (`if (group_vector) ... return group_vector;`) and general.c:1369 (`if (group_iarray) ... return group_iarray;`) — both short-circuit and hand a fresh bash the previous invocation's group list, e.g. through variables.c:1690 (get_group_list for GROUPS)

*Reset* Drop both to 0 in the same helper that clears group_array/ngroups. They are function statics, so general.c needs a tiny aok_reset_group_cache(). Leaks ngroups*sizeof(char*) plus the itos strings per invocation — negligible against handing the wrong $GROUPS to a shell running as a different user.

### `hashed_filenames` — corruption

*Defined* hashcmd.c:38

*Read before assignment* hashcmd.c:137 (hash_search in phash_search), reached from findcmd.c:355 in search_for_command on the very first command a fresh bash runs

*Reset* phash_flush (); — hashcmd.c:58, exported in hashcmd.h:39, exactly what `hash -r` does (builtins/hash.def:145). Self-contained: phash_freedata owns and frees both the PATH_DATA and its ->path. Do NOT rely on PATH import to do it: initialize_shell_variables does not call stupidly_hack_special_variables, so sv_path/phash_flush (variables.c:5986) never fires at startup.

### `histignore` — corruption

*Defined* bashhist.c:76

*Read before assignment* bashhist.c:1058 and :1063-1066 — history_should_ignore reads histignore.num_ignores and histignore.ignores[i].val on the fresh shell's first saved command

*Reset* histignore.ignores = 0; histignore.num_ignores = 0; histignore.last_ignoreval = 0; via a helper in bashhist.c. Note bash_history_enable (bashhist.c:311) does call sv_histignore, so this one self-heals for interactive shells that go through it — but shell_reinitialize's bash_history_reinit path does not, so a non-interactive second bash keeps the first shell's HISTIGNORE patterns.

### `history_lines_in_file` — corruption

*Defined* bashhist.c:101

*Read before assignment* bashhist.c:507 — `history_lines_in_file += history_lines_this_session` in maybe_save_shell_history — and bashhist.c:467 in maybe_append_history; both run before anything assigns it, because shell.c:809 skips load_history once shell_initialized is set

*Reset* history_lines_in_file = 0; alongside bash_clear_history (). Readline's own history_lines_read_from_file / history_lines_written_to_file (lib/readline/histfile.c) survive too and feed bashhist.c:339 and :512, so they belong in the same reset.

### `history_lines_this_session` — corruption

*Defined* bashhist.c:98

*Read before assignment* shell.c:809 — `if (shell_initialized == 0 && history_lines_this_session == 0) load_history ();` — and bashhist.c:506, `append_history (history_lines_this_session, hf)` in maybe_save_shell_history

*Reset* bash_clear_history (); — bashhist.c:349, declared bashhist.h:71. It calls readline's clear_history() and zeroes this counter together, which is the point: the readline history list is not cleared on re-entry either, so a stale count plus a stale list makes the second shell append the first shell's lines to ~/.bash_history a second time. bash_history_reinit (bashhist.c:281), which shell_reinitialize already calls, does not touch either.

### `ifs_value` — corruption

*Defined* deps/bash/subst.c:155

*Read before assignment* deps/bash/subst.c:12126 `tresult = word_split (t->word, ifs_value);` in word_list_split — every unquoted expansion. Also subst.c:3147 `for (xflags = 0, s = ifs_value; s && *s; s++)`, subst.c:4695 and subst.c:4780 `quote_spaces = (ifs_value && *ifs_value == 0)`, subst.c:11809, and getifs() at subst.c:12089 which hands the freed pointer to callers outside this file.

*Reset* Same single call as ifs_var: `setifs ((SHELL_VAR *)NULL)` (subst.c:12034). Never free ifs_value — subst.c does not own it, and after a shell where IFS was unset it is the string literal " \t\n".

### `ifs_var` — corruption

*Defined* deps/bash/subst.c:154

*Read before assignment* deps/bash/subst.c:2980 `ifs = ifs_var ? value_cell (ifs_var) : (char *)0;` (string_list_dollar_star), and subst.c:12044 reads it before the next assignment completes. The dangling window runs from shell.c:2047 to the reassignment in setifs at variables.c:590, which is only reached via shell.c:580 -> shell.c:1980 initialize_shell_variables. I found no expansion on the mainline between those two points, so this is latent rather than certain — but it is one line to close and the pointer is provably freed.

*Reset* `setifs ((SHELL_VAR *)NULL)` (subst.c:12034) — drop, never free; the variable table owns it. That one call also rebuilds ifs_value, ifs_cmap, ifs_firstc and ifs_firstc_len consistently, so it covers the whole IFS block. Equivalent idiomatic alternative: add `sv_ifs ("IFS")` to reinit_special_variables (variables.c:5961), which already exists for exactly this purpose.

### `invalid_env` — corruption

*Defined* deps/bash/variables.c:128

*Read before assignment* deps/bash/variables.c:3348 bind_invalid_envvar -- sees it non-zero, skips hash_create, and binds with HASH_NOSRCH, so every invocation APPENDS a duplicate entry for the same invalid name; and variables.c:5161 maybe_make_export_env, which exports the whole accumulated table.

*Reset* invalid_env = (HASH_TABLE *)NULL; -- drop, matching the export_env precedent. bind_invalid_envvar recreates it (variables.c:3348-3349). A real free (hash_flush with free_variable_hash_data + hash_dispose) is also correct since nothing else points at these vars, but the drop is the cheaper mistake. Without it, shell N exports shell 1's invalid env names and the table grows by one duplicate per invalid name per invocation.

### `jobs` — corruption

*Defined* deps/bash/jobs.c:183

*Read before assignment* deps/bash/jobs.c:588 — stop_pipeline scans jobs[i-1] on the first command a fresh bash runs, gated only by js.j_jobslots; then cleanup_dead_jobs (jobs.c:1229) -> delete_job frees temp->wd (1415), discard_pipeline(temp->pipe) (1416) and temp itself (1435)

*Reset* jobs = (JOB **)NULL — but ONLY in lockstep with js (next entry). js.j_jobslots is the sole guard on every jobs[] access, so NULLing jobs while leaving js.j_jobslots > 0 turns jobs.c:588 into a NULL deref. Drop, do not free: delete_all_jobs(0) is the freeing alternative but it also prints 'deleting stopped job N' warnings (jobs.c:1396-1397) at the start of every bash, and it leaves jobs dangling anyway (jobs.c:4814 frees without NULLing).

### `js (struct jobstats, incl. j_lastmade / j_lastasync)` — corruption

*Defined* deps/bash/jobs.c:175

*Read before assignment* deps/bash/jobs.c:570 — stop_pipeline reads js.j_jobslots before any assignment; cleanup_dead_jobs (1216, 1222), map_over_jobs (1604), mark_dead_jobs_as_notified (4992). init_job_stats() (jobs.c:373) is NOT on the startup path: shell_initialize calls only initialize_job_control (shell.c:1984), and the only init_job_stats caller is initialize_subshell (execute_cmd.c:5934).

*Reset* init_job_stats () — the existing helper at jobs.c:373 (js = zerojs) — called together with `jobs = NULL`. Resetting js alone merely leaks the array; resetting jobs alone crashes. Carrying both forward hands bash #2 bash #1's job table with pids from a dead guest task, which hangup_all_jobs (jobs.c:1672) and terminate_stopped_jobs (jobs.c:1652) then killpg().

### `local_index / local_bufused (and localbuf)` — corruption

*Defined* deps/bash/input.c:65-66

*Read before assignment* deps/bash/input.c:80 - `if (local_index == local_bufused || local_bufused == 0)` is FALSE when a tail was left behind, so no read(2) happens and input.c:115 hands the previous shell's leftover bytes to the new shell's parser as its first characters. Stale bytes from a dead shell become commands the new shell executes.

*Reset* local_index = local_bufused = 0.

### `nfds` — corruption

*Defined* deps/bash/subst.c:6230

*Read before assignment* deps/bash/subst.c:6330 `if (nfds == 0) return;` in unlink_fifo_list, reached from eval.c:78 before the fresh bash runs anything. A stale non-zero nfds is the gate that lets every loop over the stale dev_fd_list run; conversely a stale zero hides live marks from clear_fifo_list (subst.c:6249) and copy_fifo_list (subst.c:6264).

*Reset* Covered by `clear_fifo_list ()` (subst.c:6244). If reset by hand, `nfds = 0` must be paired with zeroing dev_fd_list — the two disagreeing is worse than either being stale.

### `parser_state` — corruption

*Defined* deps/bash/parse.y:275 (compiled as deps/bash/y.tab.c:325)

*Read before assignment* deps/bash/parse.y:3395 - read_token: `if ((parser_state & (PST_CONDCMD|PST_CONDEXPR)) == PST_CONDCMD)` calls parse_cond_command() on the new shell's very first token. Also parse.y:2907 (PST_EOFTOKEN) and parse.y:3322-3326, where stale PST_EXTPAT/PST_CMDSUBST/PST_STRING bits make reset_parser clobber extended_glob and expand_aliases later on.

*Reset* parser_state = 0.

### `pidstat_table` — corruption

*Defined* deps/bash/jobs.c:177

*Read before assignment* deps/bash/jobs.c:818 — pshash_getbucket, reached from bgp_search (jobs.c:954) and bgp_delete (jobs.c:901) on the first `wait` or first child of a fresh bash

*Reset* None on its own — it is rebuilt to NO_PIDSTAT by bgp_resize when bgpids.nalloc == 0 (jobs.c:764-768), which bgp_clear() guarantees. Listed because the two must never be reset independently: clearing this table while bgpids stays populated orphans every arena cell (bgpids.npid stays non-zero, every search misses), and clearing bgpids without it is only safe because of that nalloc == 0 rebuild. Note bgp_clear itself does NOT touch this table (jobs.c:926-939).

### `pipeline_pgrp` — corruption

*Defined* deps/bash/jobs.c:203

*Read before assignment* deps/bash/jobs.c:2140-2142 — aok_register_spawned does `if (pipeline_pgrp == 0) pipeline_pgrp = pid; setpgid (pid, pipeline_pgrp);` for the FIRST child of a fresh bash. initialize_job_control (jobs.c:4441-4588) never touches pipeline_pgrp — I read the whole function; the only reset is set_job_control's `if (job_control != old && job_control) pipeline_pgrp = 0` (jobs.c:5059-5060), which does not fire because initialize_job_control assigns job_control directly at 4538.

*Reset* pipeline_pgrp = (pid_t)0; — no memory involved, but a stale value puts bash #2's first child into a process group belonging to a dead (or recycled) guest task, and stop_pipeline then records it as newjob->pgrp (jobs.c:645) so every later killpg (jobs.c:1636, 1652, 1672, 3708) targets a foreign pgrp.

### `posparam_count` — corruption

*Defined* deps/bash/variables.c:161

*Read before assignment* deps/bash/builtins/common.c:462 number_of_args -- inherits the previous shell's $# when bind_args skipped remember_args (shell.c:1496).

*Reset* covered by clear_dollar_vars () (variables.c:5639), which sets posparam_count = 0. Must be reset together with dollar_vars/rest_of_args or $# disagrees with $1..$9.

### `procsubs (struct procchain)` — corruption

*Defined* deps/bash/jobs.c:180

*Read before assignment* deps/bash/jobs.c:1734 — find_pipeline walks it via procsub_search; procsub_prune (jobs.c:1139-1161, called from cleanup_dead_jobs at jobs.c:1234) walks it and frees PS_DONE nodes at 1156; procsub_waitall (jobs.c:1105-1110, called from wait_for_background_pids at 2771) calls wait_for on every stale pid.

*Reset* procsubs.head = procsubs.end = 0; procsubs.nproc = 0; — drop without freeing. bash's own procsub_clear() (jobs.c:1114) is the freeing alternative and is safe on ownership, but it aliases last_procsub_child (jobs.c:234), which it does not clear; if you call it, clear last_procsub_child in the same breath. exit_shell never calls procsub_clear (only the termsig path, sig.c:614), so a normally-exiting bash leaves this list populated with pids from a dead task.

### `prog_completes` — corruption

*Defined* pcomplib.c:44

*Read before assignment* pcomplib.c:208 (hash_search in progcomp_search), from bashline.c:1663-1664 on the fresh shell's first TAB

*Reset* progcomp_flush (); — pcomplib.c:139, exported pcomplete.h:155, exactly what `complete -r` with no args does (builtins/complete.def:438). Ownership here is unambiguous (refcount + own strings), so freeing is correct. This one is worse than a stale cache: shell_reinitialize deletes shell_functions, so every surviving `complete -F _foo bar` names a function that no longer exists, and bash-completion's dynamic loader sees the specs are already registered and never re-sources them — completion is silently dead for the whole second session.

### `pushed_string_list` — corruption

*Defined* deps/bash/parse.y:1909 (compiled as deps/bash/y.tab.c:4224)

*Read before assignment* deps/bash/parse.y:2355-2356 - shell_getc tests pushed_string_list before anything assigns it; when the stale line is exhausted the alias block calls pop_string (parse.y:1965), which FREEs the current shell_input_line, adopts the dead one, and at parse.y:1988 writes `t->expander->flags &= ~AL_BEINGEXPANDED` through a pointer to an alias record the previous invocation may have freed (unalias, or an alias table teardown). reset_parser (parse.y:3332-3333) reaches the same code through free_string_list.

*Reset* Drop without freeing: pushed_string_list = (STRING_SAVER *)NULL. Do NOT call free_string_list() -- that is the function which dereferences ->expander (parse.y:2006-2007) and frees ->saved_line.

### `queue_sigchld` — corruption

*Defined* deps/bash/jobs.c:318

*Read before assignment* deps/bash/jobs.c:3821 — sigchld_handler: `if (queue_sigchld == 0) n = waitchld (-1, 0);`. A stale non-zero value means a fresh bash NEVER reaps a child from the SIGCHLD handler, so children stay zombies and foreground waits hang; UNQUEUE_SIGCHLD then drives the count negative.

*Reset* queue_sigchld = 0; — bash sets exactly this itself on its longjmp cleanup path, wait_sigint_cleanup (jobs.c:2794-2799), which is the precedent. Left non-zero when bash #1 exits from inside a QUEUE_SIGCHLD region (notify_of_job_status jobs.c:4325, cleanup_dead_jobs jobs.c:1219).

### `redirection_undo_list` — corruption

*Defined* deps/bash/execute_cmd.c:235

*Read before assignment* deps/bash/redir.c:251-254 -- do_redirections() opens with `if (flags & RX_UNDOABLE) { if (redirection_undo_list) { dispose_redirects (redirection_undo_list); ... } }`, a free before any assignment, on the fresh shell's FIRST redirection. Worse than the free: execute_cmd.c:479-486 undo_partial_redirects() and :470-476 cleanup_redirects() do `do_redirections (list, RX_ACTIVE)` first -- they REPLAY the previous shell's saved fd moves, dup2'ing and closing fd numbers the fresh shell now owns for something else.

*Reset* redirection_undo_list = (REDIRECT *)NULL; -- drop without freeing, and specifically do NOT call undo_partial_redirects()/dispose_partial_redirects(): the first replays stale fd moves, and the second frees a list whose provenance you cannot confirm. Cost is a handful of REDIRECT nodes per invocation.

### `rest_of_args` — corruption

*Defined* deps/bash/variables.c:160

*Read before assignment* read by every "$@"/"$*" expansion and by builtins/common.c:415 list_length; same gap as dollar_vars -- builtins/common.c:411 `if (destructive || list)' only runs when bind_args called remember_args, which shell.c:1496 skips when the shell has no operands.

*Reset* covered by clear_dollar_vars () (variables.c:5633), which does dispose_words (rest_of_args) then rest_of_args = NULL. The list is always a private copy, so freeing is safe; if you prefer the drop, `rest_of_args = (WORD_LIST *)NULL' leaks one word list per invocation.

### `sh_coproc` — corruption

*Defined* deps/bash/execute_cmd.c:1790

*Read before assignment* Several, all before any assignment. (1) deps/bash/execute_cmd.c:2141-2151 coproc_reap(), called from jobs.c:1239 (cleanup_dead_jobs, which runs constantly) and jobs.c:3369: if the stale c_flags still has COPROC_DEAD it calls coproc_dispose -> coproc_unsetvars + FREE(cp->c_name) + coproc_close, and coproc_close (:2113-2128) does close(c_rfd)/close(c_wfd) on descriptor numbers the fresh shell now owns. (2) execute_cmd.c:1631 coproc_closeall() (reachable via execute_command_internal:624 without any fork, since AOK's make_child spawns and never returns 0). (3) execute_cmd.c:2194-2203 coproc_fdchk(), called from redir.c:1219 and redir.c:1246 -- i.e. on any redirection -- which reaches coproc_setvars and recreates COPROC/COPROC_PID variables in the fresh shell from the stale name. (4) shell.c:1017 coproc_flush() at exit. (5) jobs.c:3946 coproc_pidchk() can match a genuinely new child's pid against the stale c_pid.

*Reset* coproc_init (&sh_coproc); -- bash's own helper at execute_cmd.c:2037-2046. It drops c_name without freeing and sets c_rfd/c_wfd to -1 without closing, which is precisely the export_env precedent, and is already used exactly this way at execute_cmd.c:2406. Do NOT use coproc_flush()/coproc_dispose(): those free the name and close the descriptors.

### `shell_function_defs` — corruption

*Defined* deps/bash/variables.c:133 (DEBUGGER is defined -- generated/config.h:156)

*Read before assignment* deps/bash/variables.c:3566 bind_function_def, via find_function_def (variables.c:2500). With flags == 0 it finds shell 1's stale definition and returns at 3573 without recording shell 2's, so BASH_SOURCE/BASH_LINENO/`declare -F' report the previous shell's file and line for any same-named function.

*Reset* shell_function_defs = (HASH_TABLE *)NULL; -- drop; create_variable_tables recreates it at variables.c:354-355 on the next initialize_shell_variables. A proper free would need a new hash_flush free-func for FUNCTION_DEF (none exists today), which is exactly the kind of ambiguity the drop precedent avoids.

### `shell_input_line (+ _index, _size, _len, _terminator, _property, _propsize)` — corruption

*Defined* deps/bash/parse.y:284-290 and :221-222 (compiled as deps/bash/y.tab.c:334-340, :271-272)

*Read before assignment* deps/bash/parse.y:2355 then 2597 - shell_getc skips reading a new line whenever shell_input_line[shell_input_line_index] is non-zero, and returns `uc = shell_input_line[shell_input_line_index]`: the tail of the dead shell's last line becomes the new shell's first input. The size/len/property pair must move with the pointer or set_line_mbstate (parse.y:6783-6786) and RESIZE_MALLOCED_BUFFER will size against the wrong buffer.

*Reset* Drop the whole family together: shell_input_line = NULL; shell_input_line_index = shell_input_line_size = shell_input_line_len = 0; shell_input_line_terminator = 0; shell_input_line_property = NULL; shell_input_line_propsize = 0. Freeing the two buffers first is also safe here (neither is ever dangling), but drop matches the export_env precedent and costs one line buffer per invocation.

### `shopt_alist / shopt_ind / shopt_len` — corruption

*Defined* deps/bash/shell.c:310-311

*Read before assignment* deps/bash/shell.c:575-576 - `if (shopt_alist) run_shopt_alist ();` fires in a fresh bash that was given no -O at all, and shopt_setopt is handed the previous invocation's argv strings; shopt_ind having survived means add_shopt_to_alist also appends after the dead entries rather than at 0.

*Reset* free (shopt_alist); shopt_alist = 0; shopt_ind = shopt_len = 0; -- safe inside shell_reinitialize, which runs at shell.c:461, before parse_shell_options at 493 fills the alist for this invocation. The array itself is unambiguously owned here; only its .word members are borrowed, and those are not freed.

### `sigchld` — corruption

*Defined* deps/bash/jobs.c:317

*Read before assignment* deps/bash/jobs.c:3857 — waitchld: `if (sigchld || block == 0)` chooses WNOHANG over a blocking wait; also the loop condition at jobs.c:3990

*Reset* sigchld = 0; — reset with queue_sigchld. A stale positive count makes a fresh bash's first blocking wait spin non-blocking and return before the child is reaped.

### `temporary_env` — corruption

*Defined* deps/bash/variables.c:151

*Read before assignment* deps/bash/variables.c:3272 (bind_variable: `if (temporary_env && value) bind_tempenv_variable'), variables.c:4523 and 4541 (hash_lookup into it), variables.c:5139/5152 (maybe_make_export_env folds it into the exported environment).

*Reset* flush_temporary_env (); -- the existing helper at variables.c:4737 hash_flushes with free_variable_hash_data, hash_disposes and NULLs. It is exact here because a non-NULL temporary_env always owns its table. Conservative drop: temporary_env = (HASH_TABLE *)NULL;. Without either, a bash that died mid-`VAR=x cmd' hands its assignment prefixes to the NEXT shell, which will see them via find_variable_tempenv and export them.

### `the_pipeline` — corruption

*Defined* deps/bash/jobs.c:219

*Read before assignment* deps/bash/jobs.c:525 — start_pipeline: `if (the_pipeline) cleanup_the_pipeline ();` -> discard_pipeline (jobs.c:1463) frees every node and its command. Also read by find_pipeline (jobs.c:1725) before that, which can match a stale pid and hand a leftover PROCESS to wait_for.

*Reset* the_pipeline = (PROCESS *)NULL; — drop, do not discard. Left non-NULL when bash #1 exits between make_child and stop_pipeline. Note the interaction with already_making_children below: with that flag stale at 1, making_children (jobs.c:423) returns early, start_pipeline never runs, and bash #2's add_process grafts its own nodes onto bash #1's ring instead of cleaning it.

### `the_printed_command_except_trap` — corruption

*Defined* deps/bash/execute_cmd.c:215

*Read before assignment* Freed before assignment at deps/bash/execute_cmd.c:4407 (`FREE (the_printed_command_except_trap);` at the top of execute_simple_command) and identically at :672, :2942, :3078, :3429, :3569, :3822, :4054, plus builtins/evalstring.c:83. More serious than the free is the READ at execute_cmd.c:4457: AOK sets `aok_fork_cmdtext = the_printed_command_except_trap`, and jobs.c:2221 hands that text to aok_spawn_command(), which RE-RUNS it as the child. Line :4407 normally refreshes it first, but it is skipped when `signal_in_progress (DEBUG_TRAP)` or `running_trap` is set -- and both of those are trap.c globals that survive the same way -- so a fresh bash can spawn the previous shell's command line. Also dereferenced unguarded at :4461 (savestring) and :2668.

*Reset* the_printed_command_except_trap = (char *)NULL; -- drop without freeing (one leaked string per invocation). NULL is a value bash already assigns itself at :4408, so nothing downstream is surprised by it.


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

## One more defect, from the same audit

**A failed spawn is reported to the script as success.**
`execute_cmd.c` ~5681: when `aok_spawn_disk_command` returns -1 the code sets
`p = NULL` and carries on. Upstream's fork-failure path — `sys_error("fork")`,
`terminate_current_pipeline()`, `throw_to_top_level()` — is never reached, so a
command that could not be started looks like one that ran and succeeded.

That is the silent-wrongness shape this codebase exists to avoid, and it should
be fixed alongside the re-entry globals. Verified by the audit against the
source; the exit-status path itself is otherwise correct — `/bin/false; echo $?`
now prints 1 and `/bin/true` prints 0, which is the check that the wait fix
works (before it, `false` reported 0).
