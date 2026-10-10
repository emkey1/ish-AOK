# iSH-AOK TODO

Open work: bugs that are diagnosed but not fixed, reported issues, features
deferred on purpose, and host capabilities worth exposing. Each entry says what
is already **established**, so nobody re-derives it, and what the **next step**
actually is.

This is a lab notebook, not a task list. It records what is *known* about work
that is not done; it does not say what will be done next or in what order.
That is [docs/roadmap.md](roadmap.md), and the near-term commitments for the
release being prepared are in `docs/build_<N>_musts.md`.

Started 2026-08-19, after the 549 release run. Closed entries from the 549 and
550 cycles moved to
[docs/historical/todo-closed-549-550.md](historical/todo-closed-549-550.md) on
2026-09-07.

---

## Queued for a future release

The one queue for follow-up work. Sessions add a bullet here instead of
raising task chips (the maintainer's rule, 2026-09-26): what was seen, the
evidence, and the proposed fix. Check here, and `git log origin/working`,
before starting anything. When a session, chip or subagent takes an item,
put `**[in progress: <who>, <date>]**` at the start of its bullet and commit
that before the work; when the work lands, delete the bullet, since the
commit is the record. The first ten were chips not started before the 556
freeze (2026-09-25); their full text is in
[future-release-queue.md](future-release-queue.md).

- **A 557 crash in sockrestart_on_resume** (Organizer, 2026-10-07, SIGSEGV writing 0xfffffffffffffffc; symbolicated with the 557 archive's dSYM): fs/sockrestart.c:521, `task->sockrestart.punt = true`, reached through a `listen_tasks` node whose `next` was NULL (`punt` is at offset 4). Suspected: a task leaves the list with its count unbalanced and is then freed -- nothing removes a task from `listen_tasks` at teardown, and poll.c pairs begin/end over a `poll_fds` list another thread's epoll_ctl can change in between. A CLI probe of that shape did not crash. Next: remove the task at exit (and assert the count is zero), then a probe that changes the epoll set during a poll across a suspend.
- **execve, three small gaps left by fd70bb30b:** native `sysconf(_SC_ARG_MAX)` (kernel/native_libc.c nlibc_sysconf) returns EINVAL, so native tools get no ARG_MAX; a guest execve with argv == NULL is EFAULT where Linux 5.10 runs it with an empty argv; and execveat on a bad fd reports E2BIG/EBADF in a slightly different order from Linux.
- **The standalone `virgl_render_server` no longer links** (`ninja -C build` all targets): `_mvkAOKSetNextImageRowPitch`, the MoltenVK hook from the 557 cycle's dumb-buffer pitch fix, is referenced by libvirglrenderer's vkCreateImage but not linked into the server. The app runs virglrenderer in process and is unaffected; give the server the hook (or a weak no-op) so a bare `ninja` passes again.
- **GH #631, ssh sessions drop when the device idles.** No work this cycle and no entry until now. The likely shape is iOS suspending the app and taking its sockets; the location background mode (Info.plist UIBackgroundModes) is the existing keep-alive. Reproduce on a device with the screen locked, then say in the issue what does and does not keep a session up.
- **The theme toolbar button has no accessibility label.** PR #630 labelled the preview image inside it, which VoiceOver never reads separately (a UIButton is one element); label the button itself, and localise it (app/*.xcstrings).
- **The File Provider shows as "iSH" in Files** (app/FileProvider/Info.plist display name), not iSH-AOK. opt/AOK/docs/files-app-integration.md now says what it really shows; rename it, and the doc with it.
- **Settings.bundle: the Swap Size picker sits after the Suspend group** in app/Settings.bundle/Root.plist, so it may appear under the wrong header. Check in Settings on a device and move it.
- **README and book ch41 quote ~6.8 ns per dispatch.** That figure predates this cycle's gadget work and may be stale; re-measure before the next release's docs pass.
- **networking.md's advice to bind ports >= 1024** rests on hosts refusing privileged binds; one M4 iPad allowed one (tests/manual/inaddr_any_iface.c). Check what iOS 26/27 devices do and narrow the advice to the evidence.
- **Two local branches hold unlanded work** (sweep for 558, nothing deleted): `claude/jovial-dhawan-96001a` (6c0568e83, 2026-08-22) -- the #523 "Go heap-growth stall" diagnosis, which docs/TODO.md's #523 entry does not contain (an `alloc` probe: growing the Go heap by 256 MB took 0.2 s best, 12 s typically); and `worktree-external-display-540` (6156597ee, 2026-07-30), the parked external display mirroring (GH #540). Decide whether to land the #523 text and when to raise #540.
- **Appendix F has 27 tests with a blank description** -- their sources start `/*` with the text on the next line, which docs/book/appendices/generate.py's first_comment() does not read. Shipped ones: eventfd_interrupt, futex_core, process_lifecycle, pthread_sync, signal_altstack, signal_core, signal_poll, signal_realtime, signal_restart, x86/amd64_regress, x86/atomic_cmpxchg32, x86/atomic_cmpxchg8b, x86/atomic_logic32, x86/atomic_xadd32, x86/atomics32. Give each a one-line `//` header, or teach first_comment() the two-line form.

- **Guest instructions still implemented in C (inventory 2026-10-05; the maintainer's rule is 100% gadgets).** riscv64: its gadgets call C for infrastructure (tlb_handle_miss, tlb_write_ptr_slow, cross-page access, hle_call, fence.i's flush) and, through call_helper, for AOK_VCLOCK (the vDSO's clock read, a kernel service as a system call is); fclass and fcvtmod.w.d are gadgets since 2026-10-05, and since 2026-10-07 the FP CSRs fflags/frm/fcsr (fp.S csr_fp, emu/fpenv.c's model in asm), the cycle/time/instret counters (csr_counter, the host counter as ns; counter_ns.c covers riscv64 too) and the vendor-extension pack (jit/riscv64_vendor_ext.c now rewrites a registered custom-0..3 word into the ratified instruction it equals, compiled with that instruction's gadgets; riscv64_vendor_ext_dispatch and its C handlers are deleted; opt/AOK/docs/riscv64-vendor-extensions.md updated). arm64: MRS/MSR FPCR/FPSR are gadgets since 2026-10-07 (jit/guest-arm64/dpextra.S; fpenv_arm64_sysreg deleted; arm64_fp_env catches a mutated one), so no arm64 instruction runs in C. Atomics are gadgets since 5680ff71, LD1-4/ST1-4 since 8e4937bc, the SHA512 and CRC32 fallbacks since 076e3744, MOPS since 2026-10-06; the pair helpers remain only for emu/arm64_interp.c. x86, inventoried 2026-10-06 (~25k lines of run-time C semantics; on Python/sed/sort/gzip workloads it is 2-4% of host time, so this is coverage, not speed). Mechanisms: misc.S's helper_*/fhelper_*/vec_helper_* gadgets (blr to a C pointer from gen.c's h()/hh()/h_read()/fh()/v() macros), amd64's helper_tlb_N_retint bridges to amd64_jit_* (emu/amd64_interp.c), direct `bl`s, and the amd64 interpreter fallback. **amd64, gadgets since 2026-10-06:** POPCNT, CMPXCHG to memory (32/64), PUSHF/POPF (16/64; TF now traps after the next instruction), RET imm16, POP r/m, MOV moffs, 16-bit PUSH/POP/PUSH imm/LEAVE and REX.W PUSH/POP (were interpreter-only), LOOP/JECXZ with 0x67, MOVS/STOS/LODS/SCAS/CMPS (every size, REP/REPE/REPNE, 0x67, FS/GS, page-span bulk path), CPUID (asm lookup in emu/cpuid.h's tables) and XGETBV (#GP at the instruction now). Also since 2026-10-06: CMPXCHG and XADD to memory (every size, high byte, LOCK), the whole ALU family (add/or/adc/sbb/and/sub/xor/cmp, TEST and MOV reg-reg: every size, high bytes, r/m-reg, reg-r/m, imm, memory, LOCK -- jit/gadgets-aarch64/math.S amd64_gen_alu_mem/amd64_gen_alu_rr), every shift and rotate including RCL/RCR (memory, register, 1/CL/imm), MUL/IMUL/DIV/IDIV r/m at every width (128-bit DIV in asm, #DE; 8/16-bit AX/DX:AX forms too), INC/DEC and NOT/NEG at every width with LOCK (the general ALU gadgets: ops 10 NEG, 11 NOT, a keep-CF bit), IMUL r, r/m, imm at 16 bits, MOVZX/MOVSX into any register at any width (amd64_movx_any), PUSH r/m, XCHG with memory, and the whole 0F r/m group (CMOVcc, SETcc, BT/BTS/BTR/BTC with LOCK, SHLD/SHRD, IMUL r,r/m, BSF/BSR/TZCNT/LZCNT, CMPXCHG/XADD register forms, the hint NOPs: every width; amd64_jit_0f_rm is down to 0F AE, and the C it replaced had SHRD-by-1's OF and the 32-bit SHLD/SHRD zero-extension wrong); misaligned LOCK goes through emu/tlb.c's x86_atomic_* (infrastructure, as tlb_handle_miss is). Tests: tests/manual/x86/amd64_{alu_mem,shift,0f_rm,misc_rm,muldiv,xadd,xchg_mem,push_rm,cmpxchg}.c, the first four generated by tools/gen-amd64-{alu,shift,0f-rm,misc-rm}-test.py; C deleted: amd64_jit_grp3_muldiv, locked_alu_slow, locked_xchg_slow, imul_imm, movx, fe_group, grp3_op, and amd64_jit_0f_rm down to 0F AE; all pass on camd. ISH_TRACE_AMD64_BRIDGES=1 lists the C bridge sites a run compiles (now dumped on every new site). **amd64 SSE/SSE2/SSE3 and MMX, gadgets since 2026-10-06:** all of it in the JIT -- the integer ops (amd64_vi_*), packed shifts (amd64_vsh_*), MMX moves and odds (amd64_mmx_*), data moves (amd64_xm_*), and floating point exact to x86 (amd64_xf_*: arithmetic, MIN/MAX, SQRT, CMPcc, (U)COMIS, ADDSUB/HADD/HSUB, every conversion, RCP/RSQRT, MOVMSK; DAZ, FTZ, DE and x86's NaN rules done in the gadget, host FZ off for amd64 -- matched case by case against an AMD Ryzen under seven MXCSR modes, tests/manual/x86/amd64_sse_float.c); an encoding x86 does not have is #UD from a gadget arm (amd64_sse_ud.c checks all 1332 forms against the hardware). amd64_jit_0f_vec_rm and amd64_sse3_haddsub remain only as the interpreter's. **SSSE3/SSE4.1/SSE4.2 too, since 2026-10-06** (the 0F 38 and 0F 3A maps, MMX forms included -- those were SIGILL): amd64_vi_* kinds, PMOVSX/ZX, PALIGNR/PBLENDW/BLENDPS/PD/MPSADBW (amd64_v3a_*), PTEST/PEXTR*/PINSR*/EXTRACTPS/INSERTPS/CRC32 (amd64_s4_*; CRC32C native or bitwise without FEAT_CRC32), ROUND* and DPPS/DPPD exact to the hardware, PCMPESTRI/ESTRM/ISTRI/ISTRM as the SDM's algorithm on NEON column masks (amd64_pcmp*str*), LDMXCSR/STMXCSR (a reserved bit is #GP now; fpenv's FPCR cache compares the live register); what the maps hold that AOK does not advertise (AES, PCLMULQDQ, SHA, MOVBE, ADX) is #UD. amd64_jit_0f38 and the C string helpers are deleted. **The x87, both guests, gadgets since 2026-10-06** (jit/gadgets-aarch64/x87.S; one register-form gadget set serves i386 and amd64): every D8-DF form, FXSAVE/FXRSTOR, FWAIT, EMMS -- 80-bit arithmetic in integer registers with precision and rounding control, the real tag word (cpu->x87_valid; stack overflow and underflow), x86's NaN rules, denormal and pseudo-denormal handling, unmasked exceptions (no result for IE/DE/ZE, the 24576 exponent bias for OE/UE into a register, no store into memory) and #MF at the next waiting instruction (INT_MF, SIGFPE with Linux's si_code), FPREM's partial remainder (an AMD reduces to a multiple of 32), the environment and save images (FNSTENV masks and clears ES; FXSAVE writes only what hardware writes). Matched case by case against an AMD Ryzen: tests/manual/x86/x87_kat.c (122,880 cases, every rounding mode x precision), x87_kat_unmasked.c (27,648), x87_env.c, x87_mf.c; tools/gen-x87-test.py. 1.3-1.7x faster than the C it replaced. amd64_jit_x87, amd64_jit_x87_mem and the i386 fhelper calls for x87 are gone; emu/fpu.c remains for the amd64 interpreter and the x86_64-host backend. **The transcendentals too, since 2026-10-06** (F2XM1 FYL2X FYL2XP1 FPTAN FPATAN FSIN FCOS FSINCOS): 128-bit integer arithmetic, a leading term plus a fixed-point series with the tail joined by a sticky bit, so every result is the correctly rounded one; the x87's own 66-bit-pi argument reduction, C2 at 2^63, every special operand, stack fault and unmasked exception as an AMD Ryzen does them. The Ryzen itself is a unit off in ~2% of cases (its internal approximation, not reproducible), so tests/manual/x86/x87_trans.c (16,989 cases, tools/gen-x87-trans-test.py) allows that unit and C1 with it; the old C failed 12,083 of them. Speed about the old inexact C's (fsin 78 ns vs 66; FYL2X/FYL2XP1 4-7x faster). amd64_jit_x87_reg is deleted. Unmasked UE's tininess is now after rounding, as the hardware's (an FMUL rounding up into the smallest normal raised UE before). **VEX, both guests, gadgets since 2026-10-07** (jit/gadgets-aarch64/vex.inc, included by math.S; jit/gen.c vex_table/vex_plan, one table for gen_amd64_vex and gen_vex32): AVX/AVX2 integer, float, shuffles/permutes/blends, moves and broadcasts, widen/narrow, GPR in/out, VTEST/VPTEST/(U)COMIS, VMASKMOV/VPMASKMOV, LDMXCSR/STMXCSR/PCMPxSTRx/MASKMOVDQU, VZEROUPPER/ALL, BMI1/BMI2, FMA3 (single rounding; x86's NaN order per form, IE over DE in an invalid lane) and F16C (VCVTPS2PH's imm8 rounding; a memory destination goes write probe -> convert into VEX_STAGE -> store, so a fault leaves MXCSR untouched). x86 tininess (after rounding) is now exact in the SSE/AVX MUL/DIV/CVT/DP bodies too (math.S xf_tiny: a result that came out +-minnorm is redone scaled by 2^64), FTZ included. Tests, generated by tools/gen-vex-{int,float,imm,move,width,gpr,misc,mask,bmi,fma,f16c}-test.py and tools/gen-sse-tiny-test.py, hashed against camd: tests/manual/x86/{amd64,i386}_vex_*.c and *_sse_tiny.c (F16C: every half, all eight imm8 roundings, #UD for W1/vvvv, the fault ordering read from the signal frame). VSIB gathers too (vex.inc vex_gather_*: restartable per element, a fault leaves the elements below it done and their mask elements zeroed, as camd does; {amd64,i386}_vex_gather.c). **The VEX cutover, done 2026-10-08:** every VEX instruction is gadgets or #UD -- the amd64 C4/C5 bridge to amd64_vex_step is gone (bytes that will not decode, a 0x67 address, go to the interpreter as any undecodable instruction does) and the i386 vec_avx32 fallback is the x86_64-host backend's only. The gate, tests/manual/x86/{amd64,i386}_vex_ud.c (tools/gen-vex-ud-test.py): every VEX encoding of maps 1-3 (each opcode, pp, L, W; register, memory, and vvvv != 1111b) against an Intel CPU's answer (SDE -spr with its chip check; AOK is GenuineIntel -- an AMD Ryzen differs in the opmask instructions, AVX-VNNI, VEX GFNI/VAES/VPCLMULQDQ, which it lacks, and VPERMQ/PD W0 and FMA4, which it runs). It found: W1 forms Intel #UDs that the bridge ran (VBROADCAST*, VPBROADCAST*, VTESTPS/PD, VPERMD, VPBLENDD, VPERMILPS/PD imm, the 128-bit lane ops, VBLENDV*), FMA4, VZEROUPPER/ALL and RORX with vvvv != 1111b (RORX's check returned a runnable plan), map 1 0x43, i386 KMOVD with W1 (the SDM's N.E.: it is KMOVD), AVX-VNNI missing (now through the EVEX table), and on i386 ~4000 encodings vec_avx32 ran with the wrong pp. **The EVEX cutover, done 2026-10-08:** every EVEX instruction is gadgets or #UD too -- amd64_jit_vex, the last C bridge for VEX/EVEX, is gone (it aborted the host on EVEX 0F 54 W1 [mem]); EVEX bytes that will not read go to the interpreter as any undecodable instruction does. The gate, tests/manual/x86/{amd64,i386}_evex_ud.c (tools/gen-evex-ud-test.py): every EVEX encoding of maps 1-3, 5 and 6 (each opcode, pp, L'L, W; register and memory) against SDE -spr with its chip check, AVX512-FP16 (maps 5/6 and its map 3 rows; SPR has it, AOK neither implements nor advertises it) made #UD, and three later parts' encodings SDE's chip check lets through (3.07 pp1 TILEMOVROW, 3.08/3.26 pp3 VRNDSCALEBF16/VGETMANTBF16) made #UD. Its sweep found only gaps, now all gadgets: VMOVSS/SD, VMOVLPS/HPS/LPD/HPD/HLPS/LHPS and stores, VMOVSLDUP/SHDUP/DDUP, (V)(U)COMISS/SD, VANDPS..VXORPD, VMOVD/Q, VPINSRB/W/D/Q, VPEXTRB/W/D/Q, VEXTRACTPS, VINSERTPS, VPBROADCASTMB2Q/MW2D, VDBPSADBW, VRCP14/VRSQRT14 (Intel's published reference algorithm, recip14.c: a 64-entry table and a truncated linear step on the top 16/15 fraction bits; checked exhaustively against the reference for all 2^32 floats and against SDE), and BF16 (VCVTNEPS2BF16, VCVTNE2PS2BF16, VDPBF16PS: each product fused into the sum, DAZ, FTZ after rounding, s1's NaN first). i386 found VPINSRB/W (WIG) and VPBROADCASTD/Q r (N.E.: as W0) with W1 #UD -- fixed. **Advertised since 2026-10-08** (emu/cpuid.h CPUID_ADVERTISE_VECTOR_STATE, on for aarch64 hosts): AVX, AVX2, FMA, F16C, AVX-512 F/DQ/BW/VL/CD/IFMA/VBMI/VBMI2/VNNI/BITALG/VPOPCNTDQ, AVX512_BF16, AVX-VNNI (leaf 7 subleaf 1), GFNI, VAES, VPCLMULQDQ, AES-NI, PCLMULQDQ, XSAVE/OSXSAVE, and MOVBE, LZCNT and LAHF_LM -- glibc rates AOK x86-64-v4 (its hwcaps levels v2-v4 all "supported"), and its EVEX/AVX2 string routines measure faster under AOK than the SSE2 ones they replace (memcpy 16K ~2x, memset 2-3x). What held it back is done: XSAVE/XRSTOR as gadgets, XSAVEOPT as XSAVE (x87.S; standard format at Intel's offsets -- leaf 0x0D's table was AMD Zen 4's packing, ZMM_Hi256 sized 1024, now Sapphire Rapids': opmask 1088, ZMM_Hi256 1152, Hi16 1664, 2688 bytes; XINUSE by value), the XSAVE math frame in amd64 and i386 signal frames with Linux's magics, sigreturn's XRSTOR rules (FXSAVE-only frames, XSTATE_BV, SIGSEGV on a bad header; i386 takes x87 from the FNSAVE header, as convert_to_fxsr), handlers starting from the initial configuration, ptrace NT_X86_XSTATE (and i386's FNSAVE NT_PRFPREG/GETFPREGS, FXSAVE GETFPXREGS/NT_PRXFPREG, which were zeros), exec's reset of every vector register, AT_MINSIGSTKSZ, Linux's red zone (not on the altstack) and SIGSEGV for a frame that overruns its altstack, and MOVBE as gadgets. The i386 JIT stages operands in xmm15, so a 32-bit task's XINUSE, frame and regset cover registers 0-7 only. Tests: x86/x86_xsave.c (both ABIs; XSAVE/XRSTOR against SDE for the AVX-512 components and an AMD Ryzen, -DNO512, for x87/SSE/AVX -- SDE runs those parts on its host and loses x87 state, its MXCSR_MASK is the host's 0x2ffff), x86/x86_movbe.c (Ryzen), cpuid_xsave.c (a probe per bit, both ABIs). Still to do: device legs for the new tests (the M4 and SE2 were unavailable 2026-10-07). **AVX-512 (EVEX), decided 2026-10-07: gadgets, checked against Intel SDE 10.13.1 (-spr) on camd** (no real AVX-512 here; SDE runs 32-bit freestanding int $0x80 probes too). Done: the opmask instructions (VEX), the EVEX framework (jit/gadgets-aarch64/evex.inc: zmm0-31 by quarters, merge/zero masking, {1toN}, disp8*N, masked loads/stores with fault suppression, both decoders), the moves, the integer ops, compares/tests into k, shifts/rotates, the qword ops, VPTERNLOG, VPBLENDM, the broadcasts, the non-temporal moves, the float arithmetic and compares (masked-off lanes raise nothing; {er}/{sae}; scalar element-0 masking), and the lane-crossing ops on a whole-vector frame (VPERM*, VPERMI2/T2*, VALIGN, VSHUF*x*, VINSERT/VEXTRACT) with the in-lane imm8 shuffles; since 2026-10-08 VPMOVSX/ZX, the truncating and saturating VPMOV* down-converts (to a register, or masked to memory: only the narrow result is touched), VPCOMPRESS*/VPEXPAND* (scalar pass; register and memory), VPCONFLICT, VPLZCNT, VPOPCNT, VPMOVM2*/VPMOV*2M; and VBMI's VPMULTISHIFTQB, VBMI2's VPSHLD/VPSHRD(V), VNNI, IFMA, BITALG (VPOPCNTB/W, VPSHUFBITQMB), and GFNI, AES (AESENC/ENCLAST/DEC/DECLAST, AESIMC, AESKEYGENASSIST: ARM's AESE/AESD/AESMC/AESIMC with the round key added after) and PCLMULQDQ (PMULL) in all three encodings (EVEX; VEX and legacy SSE through the EVEX table: gen.c vex_to_evex, word bit 63 keeping an SSE form's upper bits; the SSE and VEX.128 AES/PCLMULQDQ also match camd's hardware); the gathers and scatters (evex.inc evg_*: restartable per element, as the VEX gathers); since 2026-10-08 FMA (VF[N]MADD/VF[N]MSUB 132/213/231 PS/PD/SS/SD, VFMADDSUB/VFMSUBADD: vex.inc's fma_body on the lane frame, {er}, the destination an operand), VGETEXP, VGETMANT, VRNDSCALE and VREDUCE (each {sae}), VSCALEF ({er}; musl scalbn's exact steps, one rounding), VRANGE, VFPCLASS, VFIXUPIMM, and every conversion: the same-width ones, the widening and narrowing ones (evex_cvtw/cvtn frames), VCVTSS2SD/SD2SS, to and from general registers (evex_cvt2gpr; the unsigned too), VCVTPH2PS/PS2PH (the float harness has them all) (tests/manual/x86/{amd64,i386}_avx512_{kmask,int,cmp,misc,float,perm,xform,ext,gather}.c; the float harness is the maintainer's). Two places AOK does not copy SDE: zero-masked VSUBPS/PD under round-down at 256/512 bits, where SDE writes -0.0 and the SDM 0 (the float harness checks those elements itself); and EVEX VCMP's constant predicates (0x0b/0x0f/0x1b/0x1f) raise no DE in SDE though VEX VCMP's do (SDE and an AMD Ryzen alike) -- AOK follows SDE for EVEX. A compress store / expand load faults as SDE has it, by the enabled elements' own positions, though only the packed ones are touched; AOK faults on the union of those and the packed bytes (a packed element on an unmapped page whose enabled position is mapped faults in AOK, where SDE aborts with "Could not write memory" -- the xform test skips that case under "sde"). SDE zeroes the bits above 128 for the legacy SSE GF2P8MULB/AFFINEQB (not for PADDB, PSHUFB or AESENC; -spr and -icl alike); the SDM leaves them unmodified and AOK follows it (the ext test checks them itself). SDE keeps nothing across a gather's or scatter's fault (after the restart it re-reads every element; at the fault a scatter has written nothing); the SDM has every element below the faulting one done and its opmask bit clear, as camd does for the VEX gathers, and AOK follows the SDM (the gather test checks that itself, skipped under "sde"). The legacy SSE GFNI, AES and PCLMULQDQ forms run (amd64_sse_ud expects them to; camd #UDs GFNI, having none). AES and PCLMULQDQ are advertised with the vector switch (emu/cpuid.h), and their gadgets no longer need emu/avx.c. SDE evaluates an EVEX.128 FMA's elements 128-255 too and raises their flags (VADDPS does not; nor VL 256 past 256): the float harness puts 1.0 there for its 128-bit FMA forms. SDE raises PE (and DE for a denormal) for VRNDSCALEPD/SD with imm8 bit 3, which suppresses PE (PS/SS and ROUNDSD honour it): the harness checks PE and DE clear there itself. VREDUCE as SDE has it: x - rndscale(x) rounded in the imm8 (or MXCSR) mode, inf -> +0, no PE, FTZ flushes with no flag; VGETMANT: a NaN first, then 0 and inf (1.0), then imm8 bit 3's negative NaN (IE, no DE). VSCALEF follows the SDM's table (a QNaN s1 with s2 +-inf gives +inf/+0) and sets flags as a multiply; SDE raises no OE/UE/PE under DAZ and no DE for a denormal s1 under FTZ or with OE/UE -- no AVX-512 hardware here to settle it, so the harness hashes those flags clear there. VRANGE as SDE has it: a QNaN passed over, an SNaN quieted with no sign control, two QNaNs s1's, DE beside a QNaN but not an SNaN, equal magnitudes by sign. VFPCLASS applies DAZ (SDE); negative finite excludes -0. VFIXUPIMM's "QNaN(src)" response ORs the NaN bits into whatever s1 is, and DAZ keeps the sign (SDE). Found on the way and FIXED: math.S xf_f2i_scalar raised PE beside IE for an out-of-range CVT(T)SS/SD2SI whose truncation landed in range (-2147483648.5 rounded down; camd and SDE raise IE alone) -- SSE, VEX and EVEX alike. The generated AVX-512 tests build at -O1 in the suite (-O2 took an hour in the guest for avx512_float). Device leg (iPhone SE, 2026-10-09): x86_xsave, movbe, rdtscp, cpuid_xsave, cpuid_xgetbv (both ABIs), amd64_addr32, amd64_0fae, amd64_sse_ud, the EVEX #UD and the float/gather/ext AVX-512 tests all pass; dmesg holds only their deliberate faults. Found on the way and FIXED: the tininess fix's FTZ path stored MXCSR = 0x30 for a double op (xf_lanebits_sign clobbered x9), losing the rounding, FTZ/DAZ and every mask -- sse_tiny now hashes all of MXCSR. Gaps noted: a masked fixed-size memory operand (an element broadcast's, a 128/256-bit broadcast's) is read whole, so a fully masked-off one on a bad page faults. Also found and fixed on the way: the i386 guest #UD'd any VEX with vvvv's bit 3 clear (it is ignored as a register in 32-bit mode; i386_vex_vvvv3.c). **amd64 residual bridges, deleted 2026-10-06:** amd64_jit_ff_group, reg_reg_op, reg_imm_op, modrm_imm, mem_op, shift, xchg_rm, grp3_test, cmpxchg8b and 0f_rm -- every shape they still took is a gadget arm or #UD now, checked against camd: CALL/JMP through memory with 0x66 (the TLS general-dynamic `data16 rex.W call *__tls_get_addr@GOTPCREL(%rip)`, the one real-workload hit) or FS/GS; ALU-immediate, shift and TEST register forms with a segment prefix or REX.R; MOV $imm8 to AH..BH and XCHG with AH..BH (were whole-block interpreter fallbacks); MOVSXD without REX.W (a MOV), from memory with FS/GS or 0x66; LEA with a segment prefix (ignored); CMPXCHG8B/16B (math.S amd64_cmpxchg8b_mem/16b_mem: exclusive CAS, x86_atomic_cas misaligned, 16B's #GP, only ZF); CLFLUSH/CLFLUSHOPT now fault on an unreadable line; and #UD where the C executed: LOCK on CALL/JMP/PUSH/MOV/LEA/MOVSXD/TEST/XCHG/shift and every register form, 0x66 on the fences/FXSAVE/LDMXCSR/STMXCSR, C6/C7 /1-/7. Tests amd64_ff_indirect, amd64_odd_shapes, amd64_cmpxchg16b, amd64_0fae (the old JIT failed each). Also fixed: the x86_64-host i386 CPUID/XGETBV/POPF macros (cece091f5 broke Linux CI's e2e; 43e1ce988). The segment-register moves too (MOV r/m, Sreg; MOV Sreg, r/m; PUSH/POP FS/GS: math.S amd64_sreg_*, amd64_jit_sreg deleted; amd64_segment_regs catches a mutated gadget). IRET too (amd64_iret8/4/2; it now takes TF as hardware does; amd64_iret covers IRETD/IRETW). Left in C: port_io (#GP), VEX; syscall, vmcall and ud2 are kernel entries. **amd64 interpreter only (measured 2026-10-08, every candidate probed; 0F 01 and 0x67 on a VSIB gather/scatter with a segment override closed 2026-10-09 -- amd64-mem.h amd64_vsib_addr32 truncates the element address before the segment base, amd64_addr32.c):** INT 0x80 (#UD: Linux's 32-bit compat syscall gate from 64-bit code is not provided) -- and a fallback interprets until the next syscall or 1024 steps, not one instruction. **System-descriptor instructions, gadgets since 2026-10-09 (both guests; they were #UD):** SGDT, SIDT, SMSW, SLDT and STR as Linux 5.10 spoofs them on a UMIP processor (umip.c: zero limit, bases 0xfffffffffffe0000 / 0xffffffffffff0000 or their low 32 bits, CR0_STATE 0x80050033, 0, 0x40; two bytes to memory, the operand size's memcpy'd into a register -- so a 32-bit one keeps the upper half -- a faulting store SIGSEGV/SEGV_MAPERR at the operand with the first page's bytes written; the ratelimited "umip:" kernel-log warning, kernel/calls.c x86_umip_warn), with UMIP advertised (leaf 7 ECX bit 2, cpuinfo "umip"); LAR, LSL, VERR and VERW from Linux's GDT (math.S x86_seg_desc: entries 4/5/6, the TLS entries, CPUNODE 0x7b whose limit is getcpu's; LAR returns limit 19:16 as hardware does); SWAPGS #GP; every other 0F 01 form (MONITOR, MWAIT, CLAC/STAC, XTEST, SERIALIZE, RDPKRU/WRPKRU, VMX/SVM, the AMD-only ones -- none advertised) a #UD gadget. tests/manual/x86/x86_sys_desc.c (camd for the hardware semantics and the GDT; umip.c for the spoof). **Gadgets since 2026-10-08:** XLAT (math.S amd64_xlat), HLT/INT3/INT n and every #UD and port-I/O/privileged #GP (math.S amd64_raise; amd64_jit_ud2 and amd64_jit_port_io deleted), RDTSCP and RDPID (both guests, advertised: IA32_TSC_AUX = getcpu's (node << 12) | cpu, cpu_state tsc_aux, kept current after every interrupt and by rseq_fork/rseq_exec; tests/manual/x86/x86_rdtscp.c, camd), and 0x67: on MASKMOV* ([edi]) and VSIB gathers/scatters (each element's address truncated), inert on 0F 01's register forms but MONITOR's kind, and inert where nothing has an effective address (the linker's `addr32 call` relaxation -- libcrypt has 220 -- and OpenSSL's register-only padding sent each block to the interpreter), and on a ModRM memory operand a 32-bit address (meta AMD64_JIT_MEM_ADDR32, truncated in amd64_vmem_addr and the LEA gadgets) -- the interpreter had not truncated, so an addr32 load through a pointer with bits above 31 was SIGSEGV; also MAP_32BIT, which mmap ignored (tests/manual/x86/amd64_addr32.c, camd). On the glibc root's Python/Perl/gcc/openssl/coreutils run: no fallbacks, and no C bridge but syscall and vmcall (ISH_TRACE_AMD64_JIT_STATS, ISH_TRACE_AMD64_BRIDGES). **The cycle counters, gadgets since 2026-10-06:** x86 RDTSC (i386, amd64) and arm64 MRS CNTVCT_EL0 read the host counter as ns (gadgets-generic.h host_counter_ns: ISB, CNTVCT*1e9/CNTFRQ) instead of calling clock_gettime -- the host counter does not run while the device sleeps, as CLOCK_MONOTONIC does on Darwin; tests/manual/counter_ns.c. **Misaligned LOCK, gadgets since 2026-10-09 (both guests):** math.S x86_lock_rmw does the operation -- a 16-byte block compare-exchange (ldaxp/stlxp) inside a block, the split lock across two, where only taking and releasing it (emu/tlb.c x86_lock_split_begin/_end) is C -- and each gadget takes its flags from the old value; jit/helpers.c helper_atomic_unaligned/cmpxchg8b and emu/tlb.c x86_atomic_alu/xadd are deleted (x86_atomic_rmw/cas/xchg stay for the amd64 interpreter). tests/manual/x86/x86_lock_ops.c: every locked form at every block offset and across a page against the unlocked form, camd's hash (it caught two register clobbers on the split path). Found on the way and FIXED: the i386 memory BT/BTS/BTR/BTC shifted a register bit index unsigned (a negative one faulted 512 MB away; a 16-bit one was not sign-extended) and used an imm8 index unreduced -- the x86_64-host backend too (x86_bt_mem_index.c, camd). **i386:** CPUID/XGETBV and the PUSHF/POPF/SAHF/LAHF flag collapse are asm since 2026-10-06; the x86_64-host backend jit/gadgets-x86_64 still calls helper_cpuid/xgetbv/collapse/expand. Bit ops, shifts, mul/div and push/pop are gadgets on i386, and since 2026-10-06 the BCD adjusts (exact to AMD's "undefined" flags now, i386_bcd.c; the x86_64-host helpers in jit/helpers.c are not), LOOP's ECX step, the segment-register moves (memory.S sreg_*, emu/i386_sreg.c's rules in asm; i386_jit_sreg stays for the x86_64 host), the REP MOVS/STOS page-span path (string.S, math.S's x86_str_copy/fill; jit/helpers.c rep_string_fast deleted; F2 on MOVS/STOS/LODS is REP now, was #UD; i386_string_ops.c), every SSE/MMX op and LDMXCSR/STMXCSR (math.S i386_ldmxcsr: a reserved bit is #GP now, not masked off) on an aarch64 host: the amd64 engine's gadgets through gen.c's i386_vec_map / gen_i386_vec_special / gen_i386_vec_move, a memory source staged in xmm15 or amd64_regs[0], the C path #UD there (tests/manual/x86/i386_sse_{int,shift,mov,float}.c, i386_sse4_{int,misc}.c, i386_pcmpstr.c, i386_mxcsr.c, against camd; the decoder gaps they found -- CVTPS2DQ, CVTPD2DQ, RCP/RSQRT, MASKMOVQ/DQU, MMX PEXTRW, 0F 7F to a register, the SSSE3 MMX forms, a plain 0F C2 taken as CMPPD -- are fixed). An x86_64 host still runs emu/vec.c and mmx.c. Convert family by family as V was (jit/guest-riscv64/vector.S, tools/gen-rvv-gadget-test.py): a generated test against a spec model or compiler-built scalar code, a positive control per family, then delete the C. Profile first (ISH_JIT_PROFILE / host sample) to order the x86 families by time.
- **riscv64 V at VLEN 256 (an option, not planned).** AOK's V is VLEN 128, the RVA23 minimum; vector gadgets cost a dispatch per instruction whatever the vl, so twice the elements per instruction would halve the per-element cost of long loops (Ubuntu Python spends ~3% in vector code; OpenSSL's ChaCha20 is the visible user). It touches cpu_state (riscv64_v), the signal frame's V record and the NT_RISCV_VECTOR regset (both sized by vlenb), checkpoints, vlenb, and every gadget's 128-bit mask handling (vlmask, the compare packers, vmsbf, vcpop, vfirst). Measure ChaCha20 and the vbench kernels first.
- **Native Python, so native mode can `pip install` (after 558).** Native mode (docs/native_mode_plan.md, the first 558 feature) has no package manager by design; the maintainer's answer to "I need X" there is a distro root alongside it. A CPython built as a native program (like zsh/hx, on the shim) would let `pip` fill much of that gap for pure-Python packages. **Established:** nothing yet; a native program is a function call on the guest task's thread (no fork, fork-by-relaunch only), so CPython's `os.fork`/`subprocess` need the same spawn treatment the shells got, and C-extension wheels cannot be loaded (no unsigned code on iOS), so only pure-Python packages and extensions compiled into the app would work. **Next step:** measure CPython 3.x embedded under the shim (`python -c pass`, stdlib import, `pip --version`) on the Mac CLI in a native-mode root, and list which stdlib C modules it needs built in.
- **Full code review and refactor (558 or later; not for 557).** Too many source files have grown far past a size anyone can review or hold in their head. Largest at 2026-10-01: emu/amd64_interp.c 18.0k lines, app/WorkspaceViewController.m 17.1k (one file holds the Workspace, every applet's view controller, layouts, suspend capture and more), jit/gen.c 15.8k, fs/sock.c 12.0k (inet, unix, netlink and the poll glue in one file), jit/guest-riscv64/rcache.S 10.2k, kernel/native_libc.c 9.8k, jit/gadgets-aarch64/math.S 7.7k, kernel/calls.c 7.3k, kernel/checkpoint.c 6.2k, emu/memory.c and app/AppDelegate.m 5.7k each. Plan: review each for dead code and duplicated logic first, then split along the seams already there (one applet per file; netlink and unix sockets out of sock.c; syscall tables out of calls.c), keeping behaviour identical -- moves in their own commits, separate from any change, so a bisect can tell them apart. Needs the full five-root suite and both device legs before and after.
- **#604: set up the Wayland desktop from Settings.** The desktop exists (setup-wayland.sh, setup-wayland-extras.sh, setup-gpu.sh; the Wayland applet and startup mode), but nothing in the app says so -- users find it by reading /AOK/tools. Proposed: a Settings row that opens a terminal running `sudo sh /AOK/tools/setup-wayland.sh` (optionally the extras and GPU scripts), shown until a desktop is installed, then replaced by "Open Wayland Desktop". Cinnamon itself is declined (GNOME/X11 stack, too heavy); x86_64 speed is the ongoing JIT work.
- **Done 2026-10-09: native dash matches an unmatched `[` literally.** It matched with Darwin's fnmatch, which returns an error for any pattern holding a `[` that opens no bracket expression (glibc and POSIX: the `[` matches itself), so `${t#socket:[}`, `case 'a[b' in a[b)` and `${u%[y}` all failed. kernel/dash_config_aok.h, force-included after deps/dash/config.h, undefines HAVE_FNMATCH so dash's own pmatch is used -- configured trees need no re-run. tests/manual/native_dash_patterns.c: 13 lines identical to Devuan's dash 0.5.12 (negation, classes, escapes, quoted metacharacters, globbing). start-wayland.sh's `socket:\[` workaround is harmless and stays.
- **Done 2026-10-09: native programs' fnmatch is glibc's.** kernel/native_fnmatch.c, reached through kernel/native_libc.h's `#define fnmatch nlibc_fnmatch`, follows glibc's fnmatch_loop for the C locale's collation and agrees with glibc 2.41 on all 14,960 cases of a 55-pattern x 34-name x 8-flag-set table in both C and UTF-8 locales, where Darwin's differed on 2,340 (an unclosed `[`, a trailing `\`, `[]`, `[[:bogus:]]`, an unclosed class). In a UTF-8 locale it matches by characters and then, as glibc 2.41 does, by bytes, so an invalid sequence matches rather than erroring. SmallCLUE's find, ls, grep, du, diff, tar and git, and scp, all called Darwin's; tests/manual/native_fnmatch_applets.c holds 19 GNU answers (the build before fails 13). tools/check-native-libc.py no longer lists fnmatch as pure-host.
- **SmallCLUE tar -t prints a backslash in a name bare; GNU tar escapes it** (`d/b\\` for the file `b\`, its default --quoting-style=escape). Found 2026-10-09 beside the fnmatch work; check the other quoting GNU tar applies to listed names (control characters) at the same time.
- **OpenGL on the GPU needs nullDescriptor: implement it in the MoltenVK fork.** Mesa 25.2 and later refuse to start zink without VK_EXT/KHR_robustness2's nullDescriptor ("Zink requires the nullDescriptor feature of KHR/EXT robustness2", zink_screen.c, no override; absent in 25.1.0, present from 25.2.0). iSH-AOK's GPU is Venus on our MoltenVK fork, which reports nullDescriptor = false (deps/MoltenVK, MVKDevice.mm robustness2 features; MVKCmdDraw.mm notes null vertex buffers are undefined). Measured 2026-10-02 on Alpine 3.24 (Mesa 26.1.6) in the Mac CLI and reported from an iPhone on Discord: Venus works for Vulkan and the compositor, but every OpenGL program falls back to llvmpipe, and Wayfire (GLES via zink) cannot run. Devuan 6's Mesa 25.0.7 is unaffected -- for now the only root with OpenGL on the GPU; Arch is past 25.2 too. setup-gpu.sh and setup-wayfire.sh now say so (and setup-gpu.sh installs Alpine's vulkan-loader, which nothing pulled in, and refuses Alpine <= 3.23, whose Mesa has no Venus at all). The fix: nullDescriptor in MoltenVK -- VK_NULL_HANDLE in buffer, image and texel-buffer descriptor writes (bind a zero-length or dummy Metal resource; reads return zero, writes are discarded), null vertex buffers in vkCmdBindVertexBuffers, null image views -- for both the argument-buffer and the discrete-binding paths, then flip the feature on and check zink starts (eglinfo -B -p surfaceless renderer "zink") and glmark2 runs. Check upstream MoltenVK first: it may have landed there since the fork. Venus passes the host's feature through, so nothing changes in the guest.
- **Wayfire on Alpine 3.24 (goal set 2026-10-03).** Two blockers, in this order. (1) The GPU: Alpine 3.24.2 ships Mesa 26.1.6, and since Mesa 25.2 zink refuses to start without Vulkan robustness2's nullDescriptor, which iSH-AOK's GPU (Venus on MoltenVK) does not offer -- setup-gpu.sh detects it -- and Wayfire draws only through OpenGL ES, so nothing can render until that feature exists in AOK's Venus/MoltenVK path (fixing it also unblocks Arch). (2) Packaging: Alpine has no wayfire, wf-config or wf-shell, and its wlroots is 0.19/0.20 (Devuan's Wayfire 0.9 uses 0.18). Build Wayfire 0.10 + wf-config + wf-shell 0.10 (with the menu patch, tools/wayland/build-wf-panel.sh's) against wlroots0.19 and musl in the CLI's Alpine roots (tools/build-bundled.sh), and ship them as /AOK/bundled/alpine3.24-aarch64 and -x86_64 with setup-wayfire.sh linking them on Alpine. Already musl-ready: the release guard and pixman shim (musl-aarch64/x86_64), the waybar panel option, select-desktop.sh.
- **wf-panel's menu still takes ~2 s to open on an A10X iPad (bip, 2026-10-03).** The bundled faster wf-panel (tools/wayland/build-wf-panel.sh's patch) stops the rebuild on every open: interleaved on one session, 2.1 s against stock's 4.7 s. What is left is GTK sizing and drawing the ~100-button flowbox each time the popover maps (~1.5 s of panel CPU per open; the release guard and pixman shim make no difference, A/B). Next steps if it matters: profile one open with the guest PC sampler; try keeping the popover's contents realized, smaller icons, or `menu_list` mode; waybar + wofi is the alternative today.
- **waybar logs `basic_string::_M_create` at start-up (bip, Wayfire, 2026-10-03).** A caught C++ length_error, printed between "Using CSS file" and "Output detection done: HEADLESS-1 ()" in `waybar -l debug`; the bar, its modules and the Apps/wofi button all work. Find which call throws (lldb `break set -E c++` on the CLI, or `catch throw` in gdb on device): the empty output description is the first suspect, a /proc or /sys read with a size AOK reports differently the second. Check whether labwc's waybar logs it too.
- **/AOK/bundled for the native rootfs: a Wayland desktop with no distro under it (asked 2026-10-03).** /AOK/bundled (tools/build-bundled.sh, fs/aok-bundled.manifest) now carries guest binaries built ahead of time, per distro release + CPU (devuan6-aarch64: the faster wf-panel) and per libc + CPU (glibc-/musl-aarch64/x86_64: the release guard and pixman shim), which setup scripts link instead of compiling. The same mechanism could give the AOK Native rootfs a Wayland stack it cannot install itself: a target whose programs and libraries are self-contained (built against musl and linked statically, or shipped with their own libraries under one /AOK/bundled/<target>/ prefix with RUNPATH set to it) -- compositor (labwc), foot, wayvnc, wl-present's needs, fonts and an icon theme -- and start-wayland.sh taught to find them there. Questions to settle first: app size (the generated tables embed every byte; compress?), how native-mode PATH/LD_LIBRARY_PATH reach a guest program, and whether GPU (Mesa/Venus) is in scope or software rendering only. Related (user, 2026-10-03): bundle arm64 builds only -- arm64 is the fastest guest -- and carry the libraries they need, so no root has to compile anything; then an x86_64 (or other) root could run the arm64 build too, if a guest can exec an arm64 program from a non-arm64 root (check how AOK picks the emulator per exec, and what ld.so/libc the program finds). Today x86_64 roots build the faster wf-panel themselves or pick waybar.
- **Done 2026-10-09: a pty hangup signals the session leader alone**, as Linux's tty_signal_session_leader does (SIGHUP then SIGCONT to the leader's process, send_tgid_signal); the foreground group hears at the leader's exit, SIGHUP and SIGCONT both when the terminal was already hung up (disassociate_ctty's tty_old_pgrp branch). TIOCNOTTY still signals the foreground group. tests/manual/tty_hangup_leader_only.c: a leader blocking SIGHUP has it pending and its child in the same group lives, then dies of SIGHUP when the leader exits (camd 64/32-bit; the build before fails both).
- **A native-dash background job held its caller's ssh session open (2026-10-02).** On bip, `setsid wf-panel > log 2>&1 < /dev/null &` inside a script run by `ssh host 'sh script'` (native dash) kept the ssh connection open after the script exited: the job's fork-handoff child (`script --aok-fork N`, now an exec stand-in for setsid) was still alive and evidently held the session's stdout pipe, though the job's own stdout was redirected. On Linux the connection closes when the script exits. Suspect fds the relaunched child inherits from the parent dash beyond 0-2 (its saved copies at 10+, or descriptors not marked close-on-exec). Repro: the above against any native-dash guest over ssh; `ls -l /proc/<child>/fd` shows which.
- **Done 2026-10-09: anon_inode link names are spelled as Linux spells them**: fs/adhoc.c prints `anon_inode:<class>` as given, with the brackets in the classes that have them on Linux 6.12 (`[eventfd]`, `[eventpoll]`, `[signalfd]`, `[timerfd]`, `[pidfd]`, `[fscontext]`) and not in `inotify` and `sync_file`; a dma-buf reads `/dmabuf:` (its d_dname with no name set). tests/manual/anon_inode_names.c (camd), and virtgpu_node.c no longer accepts the bracketed sync_file.
- **`finger`, `w` and `last` in SmallCLUE (asked 2026-10-02); `who` and `users` are done.** SmallCLUE 5a6c4bb added utmp_rec.c (the guest's Linux utmp/wtmp layout by offset -- 384 bytes on x86, 400 on aarch64/riscv64, chosen by uname), who (-b -r -q -H -u -m, `who am i`, FILE) and users; login records sessions, init records boot, runlevel, logouts on reap, and shutdown. Still to write, all reading through utmp_rec.c: finger -- no argument lists logged-in users (Login, Name, Tty, Idle from the tty's atime, Login Time, Where); `finger user` gives the long form (login, GECOS name and office fields, home, shell, each session, last login from wtmp, mail status, ~/.plan, ~/.project, ~/.pgpkey when readable); -l -s -m -p; no network finger (not wanted). w -- uptime's header line, then USER TTY FROM LOGIN@ IDLE JCPU PCPU WHAT (JCPU/PCPU and WHAT from /proc/<pid>/stat of the tty's foreground process group). last -- wtmp backwards: user, tty, host, login time, logout time or "still logged in"/"down"/"crash", duration; reboot entries; -n, -x (shutdown/runlevel), a FILE (-f); pair a USER_PROCESS with the next DEAD_PROCESS on the same line, as util-linux does. Note Debian 13's own `last` reads wtmpdb (sqlite), not /var/log/wtmp, unless given -f. Oracle: Debian procps w, util-linux last, Debian finger.
- **Distro roots lose the app terminal's utmp record to the guest's boot.** Found 2026-10-02 on the M4's Devuan6-arm64: util-linux login (the app's `login -- mke`, pid 3, pts/1) did write USER_PROCESS to utmp and wtmp at 16:38:50, and the guest's early boot reset utmp four seconds later (bootmisc truncates /var/run/utmp, and /run can be a fresh tmpfs), so `who` showed only the Wayland foot terminal. The app opens its terminal before the guest's early boot runs -- the same race start-wayland.sh repairs for /tmp (aok_repair_after_early_boot). Native roots are not affected (SmallCLUE's init keeps logins newer than the boot). Options: (a) once the early boot is over, restore live sessions to utmp from wtmp -- a USER_PROCESS since boot whose pid is alive and has no later DEAD_PROCESS on its line -- run by something that outlives early boot (a small native helper the app starts with the terminal, or a profile.d snippet AOK writes per boot that checks once and exits); (b) have the app's terminal login re-record itself after early boot. Whatever is chosen must write the guest's layout (utmp_rec.c's rules) and take the same fcntl lock glibc does.
- **Xfce's desktop draws no backdrop (setup-xfce.sh, 2026-10-02).** In the Xfce 4.20 Wayland session (labwc --session xfce4-session, start-wayland.sh) xfdesktop draws its icons but no background at all: black behind them, on the M4 and in the CLI. Established: not the image -- an SVG (with librsvg2-common's loader registered), a JPEG (xfce-blue.jpg) and a solid colour (image-style 0, rgba1 set) all stay black, set live with xfconf-query; the settings are read (xfconf-query lists /backdrop/screen0/monitorHEADLESS-1/workspace0/... as set, single-workspace-mode true), and wlr-randr names the output HEADLESS-1 with "Make: (null)". xfdesktop never wrote monitor keys of its own, which suggests libxfce4windowing found no monitor it could match (null make/model on the headless output). Separately, Debian's xfdesktop defaults to /usr/share/backgrounds/xfce/xfce-x.svg, which Debian does not ship. Next: run xfdesktop with G_MESSAGES_DEBUG=all and XFDESKTOP_DEBUG, check what XfwScreen reports for monitors under labwc headless, and compare labwc on camd (real Linux) with the same headless output; if it is the null make/model, the headless backend's output description (or the app's wlr-randr call) may be where to give it one.
- **Shortcuts Run Command: seven defects found documenting it (2026-10-02).** Found while rewriting opt/AOK/docs/shortcuts.md from the code; the doc describes today's behaviour. (1) **Output over 256 KB fails the action instead of returning it:** the reader stops at kISHShortcutOutputLimitKB (app/GuestCommandRunner.m:18), kernel/init.c run_guest_command_capture_shell then kills the child because it stopped short of EOF (~:896), the reap records SIGKILL, and ISHRunCommandIntent.swift throws "killed by signal 9" before anything looks at `outcome.truncated` (set at GuestCommandRunner.m:153, read nowhere). Fix: check truncated first and return the output with a truncation note. (2) **Output that is not valid UTF-8 returns as empty text** (GuestCommandRunner.m:155-157 `?: @""`), which a 256 KB cut through a multibyte character also triggers; decode lossily (NSString stringEncodingForData with AllowLossy and UTF-8 suggested). (3) **"Run as default user" with no uid-1000 account runs commands as root** (headlessCommandAccountName returns nil, AppDelegate.m:3026-3031); the intent's description overstates it, though the Settings footer covers it. (4) The su error text promises failure "if that account cannot log in" (GuestCommandRunner.m:124-127), but only a missing /bin/su fails; su's own refusal comes back as output. (5) A reap that gives up after 3 s leaves exited=0 and termSignal=0 (init.c:910-934), which counts as success even with Fail on Non-Zero Exit on. (6) A failed headless boot is cached by ensureBooted's dispatch_once (AppDelegate.m:3762-3790) until the app relaunches; the error text should say so. (7) Background boot resumes the newest saved session without asking (AppDelegate.m:4391-4407): intended, now documented. Verifying (1)-(5) needs the action run from the Shortcuts app on a signed device build (an unsigned simulator cannot resolve the intent's metadata).
- **Other roots appear under /AOK/roots after boot has started.** AppDelegate mounts them in ISHDispatchBootWork("boot.secondary-mounts") so a slow fakefs mount cannot trip the launch watchdog, which means /etc/rc, rc.d scripts and runit services race them: on the M4 (2026-10-02) an rc.local at uptime 0.3 s found /AOK/roots/Devuan6-arm64 empty, and it was there 1 s later. Documented in native-mode.md with a wait loop. If boot services that use other roots become common, either have init's /etc/rc wait for a "secondary mounts done" flag (a /proc/ish file), or mount the roots before the guest's pid 1 runs when there are few of them.
- **#616: optional bottom padding for the terminal.** The last rows sit in iOS's home-indicator gesture zone, which makes selecting or reading them awkward. Proposed: a Settings value (rows or points) that raises the terminal's bottom edge when no keyboard is showing -- not added on top of the keyboard inset (TerminalViewController's bottomConstraint / _updateSafeAreaCompensation). The reporter's extras (status text in the padding, a pre-typing queue) are out of scope for a first cut.
- **#620: Synaptic "freezes the display" on x86_64 -- unverified.** The "refuses to start" half is fixed (0ec68d2d, the session's own pkexec). The freeze did not reproduce on arm64 Devuan: root Synaptic loads its 68k-package cache in ~8 s of CPU and labwc stays responsive. Suspect the same load under x86_64 emulation runs long enough to starve the desktop, which reads as a freeze. Needs an x86_64 Devuan desk root (none exists; build/devuan-arm64-desk is the model): time Synaptic's start and watch labwc's CPU share. If it is only slow, nothing to fix beyond the JIT work.
- **Done 2026-10-01: native sudo takes the target's groups.** The shim's initgroups was a no-op returning success, so `sudo -u root id` from uid 1500 kept groups=0(root),1500(tester). nlibc_initgroups now sets the guest /etc/group memberships (matches `setpriv --init-groups`), and SmallCLUE's sudo treats a failure as fatal (149170a). Checked in native_coreutils.
- **Done 2026-10-01: native stdout is fully buffered off a terminal**, as glibc has it (kernel/native_libc.c nlibc_std_stream). Line-buffering every stdout made `seq 1 300000 >f` one write per line -- 0.546 s, now 0.024 s -- and put stdout ahead of stderr in a `2>&1` file. The shim flushes stdout before exec, posix_spawn, system and popen so a child's output still lands after what was printed first, and system/popen now pass argv[0] "sh" as glibc and musl do. Checked: all native_* tests on devuan-amd64 and alpine-arm64, and an 18-case ordering survey against GNU (find -exec/-ok, xargs, awk pipes and system, sed e).
- **Done 2026-10-09: file times are signed 64-bit.** struct statbuf's atime/mtime/ctime (fs/stat.h) and the tty's are sqword_t; the narrowing casts in adhoc, fuse and statx went, tmpfs's relatime comparison is signed, and i386's utime/utimes/timeval read their time_t as signed 32-bit. `touch -d @-1` reads back as -1, 2^33 survives, and i386's stat64 keeps the low 32 bits as Linux's does. tests/manual/stat_time_range.c (utimensat, utimes, utime; statx and stat; /tmp and /dev/shm), checked on camd 64- and 32-bit; the build before fails 24-42 of its checks.
- **SmallCLUE size: -Os for its cold applets (decide; measured 2026-10-01).** The redundancy pass (shared GNU helpers compiled once in src/gnu_util.c instead of per-applet static inline copies; dead code; duplicated join/buffer/write-all/filevercmp folded) took SmallCLUE's src/ from 877 KB to 788 KB at -O2. The next lever is the optimisation level: the whole of src/ at -Os was 761 KB against 877 KB at -O2 before that pass (-13%), but the biggest per-file gains are in the hot applets (sed -11.6 KB, awk -17.7 KB, grep -6 KB, tr -4.3 KB), where -O2's inlining is the point. The cold set -- core.c (top, ps, pager, markdown, dns...) -21 KB, tar -9 KB, ls -5 KB, date, stty, openssh, find, diff -- could take -Os through a per-file meson static_library split with no speed anyone would notice. Needs an A/B on ls -lR and find over a large tree first (interleaved samples), then the split in meson.build's smallclue section.
- **Check the other shadowing applets against GNU, as was done for sed.** native-links.sh puts every SmallCLUE applet ahead of the distro's, maintainer scripts included, and tools/native-applet-audit.py only checks that an applet runs, not its flags. Done: sed (9822c561, tests/manual/native_sed.c), stty (68ea706a, native_stty.c, with the pty recipe: GNU and native side by side under script(1)), ln (b07e1600, native_ln.c), rm, wc, head, tail (smallclue d1861d0), sort (652ff72), xargs (eb63922), find (d11a238), grep (31b5281), cp and mv (e97ea71), date (2c38034), chmod (8694831), ls (27544c7), diff (77189f2, every output style including -y; 8670 fuzzed pairs match), cmp and uniq (b30e26a), sed's e command (b30e26a), tr and nl (9c1f9c8), seq, touch and stat (e259192), realpath, readlink and env (0927a2c), cat, rmdir and sum (ef7a5d5), dd, od, fold and tac (7648c8f), split and du (aaba67d), gzip/gunzip/zcat (b355476), tar (6893f77), awk (92cb234, against mawk 1.3.4, Devuan's awk), all in native_coreutils.c. The 530-case survey against GNU coreutils/grep/findutils/mawk in build/devuan-amd64-test (2026-10-01) now leaves only accepted differences: Darwin's long double is double (`printf %a`, `seq 1e-400`), mawk's time-based srand seed and its odd substr() with a start below 0, x86's NaN sign (`5%0` is -nan there), env's "=x", and mawk's second message after a syntax error. Method: a TSV of commands run under PATH=GNU links and PATH=native links from the same fixture, comparing stdout, status and the tree; then a golden C test generated from GNU's results, as native_coreutils.c is. Fix each gap in smallclue upstream, then bump. Also keep in mind that a native program cannot exec in place, so a script that tracks a child's pid through `env`/`sh` gets the wrapper's pid: call /usr/bin/env and /bin/sh by path there.
- **Done 2026-10-01: native programs convert local time in the guest's zone.** kernel/native_tz.c routes localtime(_r), mktime/timelocal, ctime(_r) and tzset: TZ (zoneinfo name, path or POSIX rule) else /etc/localtime else UTC, TZif v2+ with its footer, glibc's quirks (empty TZ is "Universal", rules before 1970 use 1970's transitions, the doubled hour by glibc's offset search), without touching the host's TZ. Against GNU date in build/devuan-amd64-test: 482/483 zone/instant pairs (left: a rule string without rules before 1970, where glibc borrows posixrules). Native `ls -l` now agrees with /usr/bin/ls. The gate no longer lists those names as pure. Checked in native_coreutils.
- **GNU `tail -f` misses changes in the guest. FIXED 2026-10-01.** In build/devuan-amd64-test, `tail -f` on two files missed the second append, and `tail -F` missed a truncate-then-append and a `mv` over the followed name. The cause was not inotify: fakefs reported statfs f_type 0x66616b65 ('fake'), which GNU tail does not know, so it took the file system for a remote one and fell back to its own polling path. fakefs now reports EXT4_SUPER_MAGIC (fs/fake.c; the name in /proc/mounts stays "fake"); tail uses inotify (strace: inotify_add_watch IN_MODIFY), and its -f and -F output matches Linux on camd. realfs keeps its own type on purpose, so tail polls a host directory, which changes without inotify events.
- **Done 2026-10-01: native sudo takes leading NAME=value arguments** into the command's environment (`sudo DEBIAN_FRONTEND=noninteractive apt-get ...`), refusing LD_*, BASH_FUNC_*, IFS, ENV and BASH_ENV with real sudo's message (smallclue 149170a). Checked in native_coreutils.
- **#484: Venus readback on the A10X differs run to run.** zink itself now runs there (MoltenVK 67d3f726: extendedDynamicState was advertised although Apple3 GPUs cannot do its dynamic vertex stride, so every zink pipeline failed and binding one killed the context; Freedoom and Beneath a Steel Sky now draw correctly on bip's GPU, Doom's timedemo 79 fps). What remains from the original report: `tools/vkbench.c` (640x360, 2000 tris, 10 frames) gave a different checksum on every Venus run on the A10X (228652566, 293168948, 243401375) against lavapipe's 475822382, where the M4's Venus gives 475749031 every time. Re-measure with the current MoltenVK first; if it still varies, find which readback path (linear row alignment, a missing barrier, a fence retiring early) differs on Apple3. It reproduces in seconds with no desktop.
- **Idiom translation for code built without the newer instructions (musl, rv64gc distros) -- measured, mostly covered by HLE.** glibc picks MOPS through ifuncs; musl has no ifuncs and distro RISC-V is fixed at rv64gc, so the idea was to recognise their copy/fill loops as the JIT translates and run each as one operation. Sized 2026-09-30 with ISH_JIT_PROFILE (which now records fusion-consumed words, so loops no longer look like they end in their compare): with HLE off, pure copy/fill self-loops are 9-17% of the kernel mixes but 1.2-1.7% of a gcc compile, and nearly all of them are inside musl's own memcpy/memset -- which HLE intercepts at the function entry. With HLE on (now the default) what remains is <=0.9% everywhere (riscv64 gcc 0.6%, arm64 gcc 0.9%, riscv64 kernels 0.3%). hle.c already recognises arm64 post-index copy/fill loops (ISH_HLE_LOOPS; tests/manual/arm64/hle_loop.c); a riscv64 twin is not worth it at these numbers. Revisit only if a real workload shows open-coded copy loops outside libc. Suggested 2026-09-29.
- **Take the specialised-gadget approach to i386/amd64 (after riscv64).** The gadget stream is effectively an intermediate architecture tuned for the aarch64 host, and MOPS showed that giving it bigger operations pays off; the offset-fed arm64 gadgets showed that decoding operands at run time was most of a gadget's body. For the x86 engines the levers differ and need measuring first (ISH_JIT_PROFILE covers only arm64/riscv64 today -- extend it): flag computation (lazy flags vs the host's own NZCV), ModRM operand decode, x86's own MOPS (`rep movs`/`rep stos` -- check they already run as one span copy), and the i386 segment/16-bit paths. Suggested 2026-09-30. The natural unit to fuse on these engines is the x86 instruction itself: one i386/amd64 instruction becomes a chain of gadgets today (address, load, op, flags, store), and a gadget per common chain is the intermediate-architecture idea applied at the guest's own granularity. ISH_JIT_PROFILE now covers both x86 engines; rank the gadget chains per guest instruction first, then fuse the hot ones. Suggested 2026-09-30. **Measured 2026-09-30** (Devuan i386 python/gzip/xz profiles; tools/jitprof-report.py's x86 disassembly was misaligning chunks after any undecodable one -- fixed with ud2 separators, and the "9.5% SSE" it had shown is really <0.2%): memory-operand movs are 26-41% of instructions (already fused), ALU with a memory operand 7-10%, lea 1-7%, scaled-index operands 2-6%, push/pop 4-8%, x87 up to 3%, SSE <0.2% (so the i386 SSE memory/packed port is not worth doing). Per instruction on the A10X: mov reg 1.3 ns, load 2.4, store 3.0, add imm 5.3 -> 3.4 and cmp 5.3 -> 4.4 after the lazy-flag deposit stopped doing two load-or-store rounds on flags_res per op (constant byte store; logic ops keep AF lazy as op1 = res, op2 = 0; tests/manual/x86/alu_flags.c, checked against camd and mint), push+pop 5.7, lea with index 5.2, add reg,mem 7.5. Next: ALU reg,mem (three dispatches), lea/operands with a scaled index (four), push/pop. **Done:** 8-bit cmp/test + jcc fused (fused_cmp8/fused_test8, `jcc8` in /proc/ish/i386_jit_fuse); the ALU-with-memory class turned out to be mostly compares (cmpl imm/reg vs mem, cmpb reg vs mem -- 4.4% of gzip), and the byte forms had no jcc fusion. A10X: cmpb reg,mem+jcc 8.1 -> 6.9 ns/insn, cmpb imm+jcc 7.1 -> 6.3; gzip -6 on Devuan i386 12.79 -> 12.61 s. **Done:** the per-gadget `dmb ishld` is gone from i386 gadgets that read no guest memory (gret_nomem: register ALU/moves, immediates, address arithmetic, lea). It cannot simply move to the chain entries as it did for arm64/riscv64 (caf403bb): on x86 guests that barrier is also what orders each guest load before later accesses (x86 TSO's load->load/load->store), and removing it everywhere made atomic_lock_contended's xchg spinlock lose updates. A10X: addl imm 3.4 -> 1.75 ns, cmpl 4.4 -> 2.3, mov reg 1.35 -> 0.95, gzip -3..5%, python strings flat; M4: addl 1.08 -> 0.68, cmpl 1.46 -> 1.03. **Done:** jit_ret_chain inlined into each branch gadget (chain_ip, gadgets.h) so the chained dispatch has one indirect-branch site per gadget: A10X gzip -1.8%, xz -2.5% (3/3 interleaved), python flat; taken cmp+jcc 5.1 -> 3.9 ns but plain jmp 2.5 -> 3.0 and not-taken jcc 4.0 -> 5.0 in the microbenchmark, and call+ret unchanged. Still open: call+ret ~14 ns per instruction on the A10X (the ret-cache path works; the cost is the gadgets and chain entries), push/pop latency chains through esp and the TLB.
- **Stream layout: separate operand words, not one packed word (DONE 2026-09-30).** The vspec/ospec/lspec gadgets read each cpu_state offset as its own stream word (ldp) instead of unpacking one packed word with ubfx, which sat on the critical path. A10X per instruction 7-29% faster (add #imm 1.42 -> 1.04 ns, ldr 2.84 -> 2.63). Two app builds alternated on both iPads (same code, only the layout differs): unpacked wins or ties everywhere -- A10X kernels -2..-6%, gzip -3.5%, xz tie; M4 python -5%, column drawer -13%. The M4 register-only microbenchmark still reads 0.53 ns where the packed build read ~0.33, which real code does not show; at ~2 host cycles it is sensitive to layout, worth understanding if the M4 becomes the target. The older fusion gadgets (rmw/ldld/ldcmp, fast LDP/STP's rt|rt2 word) still pack.
- **HLE default: ON since 2026-09-30.** HLE was off by default because each intercepted call exited to the C dispatcher, so small calls got slower (PSCAL hello 46 -> 83 ms, bitwise 61 -> 190 ms). Since the success path returns through the ret/jalr cache (2026-09-30) the per-call cost halved (A10X musl memcpy 16 B: 97.6 -> 45.9 ns) and HLE wins at every size on both iPads for arm64 and riscv64 musl (A10X riscv64 memcpy 4 KiB: 10968 -> 160 ns). Whole programs, HLE off -> on: M4 Alpine arm64 shell loop -11%, sort -4%; M4 Alpine riscv64 shell loop -18%, sort -9%, sed/tr -7.5%; A10X Devuan riscv64 shell loop -15%, sort -14%; Devuan arm64 (glibc, memcpy already MOPS) and PSCAL neutral; nothing got slower. `/proc/ish/hle` switches it live. Musl/riscv64 get the most: their string functions are plain C loops.
- **riscv64 against real silicon (a RISC-V board with RVV is on order, 2026-10-06; usable in ~1-2 weeks).** Every riscv64 result so far -- the RVV gadgets (jit/guest-riscv64/vector.S, tools/gen-rvv-gadget-test.py), scalar FP, the CSRs -- is checked against spec models or compiler-built scalar code, never hardware. Once the board is set up: run the riscv64 tests natively there, and turn the model-based expectations into hardware known-answer tests, as camd does for x86 (tools/gen-x87-test.py is the pattern).
- **x87 after the gadget port (2026-10-06): what is left.** (1) Done (f992e7b54): the transcendentals are gadgets, correctly rounded, camd's answer in ~98% of cases and a unit away otherwise (x87.S). (2) FIP, FCS, FOP, FDP and FDS are not kept: FNSTENV/FNSAVE write 0 where an AMD writes the last instruction's (FXSAVE matches, as AMD drops them with no exception pending). (3) Done 2026-10-09: MMX and the x87 alias -- MMn is fp[n]'s significand (emu/cpu.h CPU_MMX; cpu_state has no mm[] any more), and every MMX instruction but EMMS first takes a pending x87 exception (x87.S mmx_wait, before it), then, once it has run, makes TOP 0 and all registers valid and gives the register it wrote an all-ones exponent (mmx_touch, after it; both emitted from one byte-level classifier, gen.c x86_mmx_dst, for both guests). So FXSAVE/XSAVE, FNSAVE/FRSTOR, the signal frame and ptrace carry MMX state, and a signal handler's MMX no longer leaks into the interrupted code (tests/manual/x86/x86_mmx_x87_alias.c, camd; 41 of its 50 checks failed before). An MMX instruction whose memory operand faults changes none of it, as on the hardware (the touch is after; checked in the same test from the signal frame). (4) The x86_64-host backend keeps the old decode.h x87 cases and emu/fpu.c (FUCOMPP catching all of DA E8-EF, FFREE a no-op, the reserved aliases missing). (5) Done (52b23a4b7): i386 PTRACE_GETFPREGS/SETFPREGS and NT_PRFPREG use the FNSAVE layout from the real state (kernel/ptrace.c get/set_user_fpregs_i386). On (2), measured 2026-10-09: an AMD (camd) stores FIP/FOP/FDP in FNSTENV/FNSAVE always and in FXSAVE/XSAVE only with an exception pending; AOK says GenuineIntel, whose SDM keeps FOP and (with FDP_EXCPTN_ONLY, which Sapphire Rapids reports) FDP only for an unmasked exception and deprecates FCS/FDS -- so the one value AOK lacks in the normal case is FIP, the last non-control x87 instruction's address. SDE is no oracle for it (it reports its own code-cache addresses). Keeping it costs a store in every non-control x87 gadget or a dispatch per instruction; the known reader is the GetPC idiom (`fldz; fnstenv [esp-12]; pop`) of shellcode and old packers. Not done for that reason. The 2026-09-30 notes below predate the port:
  **(2026-09-30)** Exact round-to-nearest fast paths for add/sub/mul/div (float80.c), amd64 x87 decoded at translation (register forms direct, memory forms from gen_amd64_decode_mem_meta), i386 scalar SSE register ops on the amd64 native gadgets. Device per-instruction now ~25-30 ns (A10X) / ~11-13 ns (M4) for fadd/fmul on both engines, fdiv 78-81 / 32-34, SSE 5.1 / 0.64. The C-call path is trimmed: i386 x87 ops call through fhelper gadgets without the eight-register spill/reload, and amd64 x87 register forms keep the register cache live across the call (memory forms store it when dirty but keep it); on the A10X that took i386 fadd 25.5 -> 23.7 ns and amd64 fadd 30.4 -> 27.0, fmul 34.2 -> 29.1, fldl+fstp 39.3 -> 34.2 (tests/manual/x86/amd64_x87_cache.c). amd64 memory forms are still ~12 ns behind i386 (EA and TLB lookup in C: move the address into the gadget, as the i386 path does). FSQRT (cb8b71a8): integer root from a double estimate, A10X 280 -> 79 ns, and C1 now clear as on AMD and Intel hardware (tests/manual/x86/x87_fsqrt.c hashes 36000 results against camd/mint). What remains beyond that is the transcendentals (fsin/fcos/fpatan already use host libm; fyl2x/f2xm1 are cheap); i386 SSE memory forms and packed ops still go through vec_helper C calls, but SSE is <0.2% of real i386 instructions (see the x86 entry above), so that is parked.
- **Revisit caching translated code across runs. Measured 2026-10-01 on the A10X, M4 and Mac.** Translations belong to one address space: threads share them, but a fork child and every exec start with none, so each process re-translates libc, ld.so and its program, and a fork child even re-translates the code its parent already had. Tool: `echo 1 > /proc/ish/jit_timing`, run, read (875e7f5c); ISH_JIT_TIMING in the CLI. The counter is global: a running Wayland desktop translates in the background (bip ~69k blocks/s idle, the M4 ~9k), so the driver samples the idle rate before each run and subtracts it -- after that both iPads translate the same block counts for the same work. Driver: guest python (root) measuring wall, children's CPU and translation time, median of 3 after a warm-up. Translation as a share of CPU, A10X (Devuan arm64, ~0.9-1.0 us a block) / M4 (~0.25-0.6 us) / Mac -O2 CLI: `sh -c true` 21% / 23% / 10-13%; 100 `( : )` forks (no exec) 14% / 19% / 26-29%; 100 fork+exec of /bin/true 19% / 19% / 21-28%; 100 `sed q` 8% / 8.5% / 24%; 100 `echo|sed|tr` pipelines 9% / 10% / 31-33%; gcc -O2 -c of a small file 4.8% / 4.3% / 5%; gcc link 7.7% / 7.8% / 10%; python3 -c pass 9.0% / 9.5% / 7%; nine stdlib imports 2.4% / 2.3% / 1.6%; apt-get -s install 0.7% / 0.8% / 0.5%; perl with four modules 3.0% / 2.8% / 3%. A real build (AOK's dash: `./configure`, then `make -jN` of 34 files; the final link fails on AOK shim symbols, which does not matter here), A10X 2 CPUs / M4 6 CPUs: configure 83 s with 9.0% translating / 23.6 s with 9.1%; make -j1 182 s, 3.9% / 63 s, 3.1%; make -jN 103 s, 3.8% / 22.9 s, 3.1%; make -j2N 82 s, 4.5% / 20.5 s, 3.5%. Parallel jobs make each translation dearer (A10X -j4: 1.22 us a block against 0.90 at -j1), but everything else slows as much, so the share barely moves. A cached block still has to be found, copied and rebased (gadget addresses change per app launch, guest addresses per ASLR), maybe 50-100 ns against 250 ns (M4) to 1 us (A10X) to translate, so a cache recovers roughly 60-90% of these shares: ~3-4% on builds, ~6-8% on configure scripts and python start-up, ~10-17% on scripts that spawn many short processes. The July Phase 0 (jit_code_cache_plan.md) set a 30% bar for building it; that bar was its own choice, and 10-20% on common shell work is worth having -- the decision is open, not closed. **Fork inheritance DONE (9e28a5f9):** a forked child copies its parent's block when the pages under it are still the same memory in both; arm64/riscv64 children also donate their own blocks back, so siblings share the paths the parent never runs (94% of a subshell's blocks copied). Interleaved A/B, inheritance off -> on, M4 / A10X / A9 (ip5): 100 `( : )` -14% / -20% / -7%; 100 `$(echo x)` -15% / noise / -5%; 100 fork+exec of /bin/true 0% / -17% / +1%; 100 `echo|sed|tr` +1% / -0.5% / -4%; dash ./configure +0.6% / -0.2% / +1.5% (noise level, 3-5 rounds). So it pays where a shell forks without exec'ing; configure and builds exec nearly every process, and those still translate from scratch -- only a cache keyed by file (across exec and across runs) reaches them, and their translation share is the ~3-9% above. A9 (ip5, Devuan arm64) shares, for the low-water mark: sh -c true 19%, fork+exec /bin/true 18%, gcc -c 5.1%, python3 start-up 9.2%, nine imports 2.4%, apt 0.7%, perl 3.1%; dash ./configure 117 s with 8.3% translating; make -j1 275 s, 4.1%; make -j2 167 s, 4.3%; make -j4 (2 CPUs) 8.5% -- oversubscribed, the per-block time includes being descheduled mid-translation, since the timer is wall clock. (ip5's native `nl` lacked -v, which broke dash's mkbuiltins under the default PATH; reported to the SmallCLUE session 2026-10-01, the runs above put /usr/bin first.)
- **Native translation of straight register-only runs (arm64 and riscv64 guests).** **Prototype 2026-10-01 (riscv64 `rcache`, off by default):** iOS forbids generating host code, so a "native run" can only be built from precompiled gadgets: every instruction still dispatches, and the lever is the gadget body. A straight run of RV64I/M ALU instructions keeps up to four guest registers in host x23-x26 -- one load gadget, per-instruction gadgets specialised by host register (`add x23, x24, x25` + dispatch; jit/guest-riscv64/rcache.S, 2214 gadgets from tools/gen-rv-rcache.py), one store gadget -- chosen per run by a cost model of A10X timings. A10X: a dependent add 2.28 -> 0.99 ns, a dependent 3-op chain 8.4 -> 4.2 ns, an independent add 1.34 -> 1.01 (the bare dispatch is ~1.0 ns). Whole programs, though: gzip -1.5..-3%, xz -1%, gcc +0.5..1%, python3 flat. Lessons: (1) runs that took apart fused pairs made them 2x slower -> the cost model counts fusions; (2) scanning ahead of every instruction made gcc ~1% slower in translation alone (a scan-only build showed it) -> the scan now starts after gen_step's own fetch and never rescans a rejected window; (3) runs in real riscv64 code are short (17% of a gcc compile's instructions sit in runs >= 3 that fit four registers, mostly 3-4 long), so the load/store gadgets eat most of the gain. Next levers: more cache registers (no gadget outside a run uses them, so x19-x22/x27 and caller-saved x3-x17 are available; six registers = ~5.6k gadgets) for longer runs, and the arm64 NEON runs in llvmpipe. **Measured 2026-10-01, NEON not worth it either:** on the M4 a dependent NEON op does pay the round trip (add .4s 1.44 ns dependent vs 0.53 independent; a dependent fmul/fadd/fmax chain 3.19 vs 1.63), but V-register-only forms are 28.6% of llvmpipe and their runs are short -- element moves, SIMD loads/stores and scalar code break them: runs >= 3 cover 5.2% of instructions with 4 cached V registers (mostly 3-4 long) and 18.4% with 8 (mostly <= 8). The 'dozens long' figure counted every SIMD/FP instruction, loads and element moves included, which a register cache cannot hold without cached-operand load/store and GPR variants (and fault paths that write the cache back). Without host code generation, the register-cache approach tops out at a few percent on both guests; not pursued further. tests/manual/riscv64/rcache_run.c (generated with its own RV64 model as the oracle) turns the bit on itself; the riscv64 suite passes with rcache on for the whole session. Found on the way and FIXED (7656ac5d): PTRACE_SINGLESTEP over any fused pair ran both instructions, on arm64 and riscv64.
   Measured 2026-09-29 (M5 Mac, build-games-o2): every guest instruction costs ~1.5-2.0 ns under the gadget JIT whatever it does -- add, ldr, umov, dup, vector fmul alike -- on BOTH engines, so time follows dynamic instruction count. `ISH_JIT_PROFILE=<file>` (jit/jitprof.c) + `tools/jitprof-report.py` give the exact mix. Findings: (1) RISC-V runs the same C source at the same per-instruction cost but needs 1.2-1.5x the instructions on scalar code and 4.9-6.3x where arm64 GCC used NEON/ldp (swizzle, audio mix, memcpy without HLE) -- the RISC-V gap is instruction count; (2) Doom on the GPU is scalar (loads 36%, SIMD 2.8%); SIMD-heavy phases are software GL (llvmpipe: blocks average 239 instructions, 61% SIMD, 41% of it in runs over 32) and sound pre-conversion at start-up; (3) register-only data processing (no memory, no branch) in straight runs is 52-73% of instructions, and one dispatch per run would remove 27% (Doom), 34% (arm64 kernels), 47% (riscv64 kernels, 64% on the swizzle) and 56% (llvmpipe) of dispatches -- more for RISC-V than arm64, so this closes the gap rather than widening it, unlike NEON-only native translation (distro RISC-V has no vectors). (4) HLE (off by default; `ISH_HLE=1`, app toggle) already takes memcpy to near native on both guests (riscv64 4.86 -> 0.24 ms per 4 MiB) -- worth reconsidering the default. Next: prototype a run translator (load the run's guest registers from cpu_state into host registers, the run's host instructions, store back) for riscv64 ALU ops and arm64 scalar+NEON, and measure against these profiles. Host `sample` is misleading for this: a stale link register after a miss handler's `bl` files later gadgets under it, and a sampling window can land on start-up. **Done since:** riscv64 ld/sd pair fusion (7772d034; a fused access ~15% cheaper, ~2% on a gcc compile) and the dispatch barrier moved from every gret to the chain entry (caf403bb). On devices (static CPU-time benchmarks, 2026-09-29): the M4 is unchanged either way (an arm64 `add` costs 0.42 ns there); the A10X, 3 alternating rounds, has register-only instructions 6-9% slower and memory instructions 7-11% faster, realistic kernels -7% to +5% -- a small net gain, far from the 10-25% a loaded Mac suggested. Measure on the devices, not the shared Mac. Remaining lever: the gadget body (operand fetch + memory-resident guest registers), about two thirds of an instruction's cost. **FEAT_MOPS (2026-09-29):** arm64 guests now get HWCAP2_MOPS and the CPY/CPYF/SET triples run as one gadget each (jit/arm64_mops.c; <=64-byte one-page cases inside the gadget, the rest in C, page span by span, fault-restartable), so Devuan glibc 2.41's memcpy/memmove/memset use them. Devuan glibc memcpy, MOPS vs NEON: Mac CLI (HLE off) 16-64 B 1.5x, 1 KiB 4x, 4 KiB 7x, 64 KiB memset 13x; M4 (HLE on) 16-64 B 21 -> 12 ns, larger equal; A10X (HLE on) 16 B-1 KiB 110 -> 53 ns, 4 KiB 1.25x, larger equal. `/proc/ish/arm64_mops` (or ISH_MOPS=0) switches the advertisement for later execs. **Next, NEON gadget bodies:** the hot SIMD instructions in llvmpipe are element moves (umov/ins/dup/mov by element, ~20% of its instructions) and three-same arithmetic, all through generic gadgets that decode register numbers, size and Q at run time (vext_to_gpr ~25 host instructions for a umov); per-arrangement gadgets fed precomputed cpu_state offsets would be ~6. **Done (vspec):** element moves (jit/guest-arm64/simd_spec.S) and every three-same/bitwise op (simd_spec3.S, generated by tools/gen-simd-spec3.py) now use such gadgets; `echo vspec=0 > /proc/ish/arm64_jit_fuse` for A/B. A10X: element moves 59-75% cheaper (umov 4.0 -> 1.06 ns), three-same 47-56% (add .4s 3.05 -> 1.34); M4: 26-68% and up to 41%. A specialised move now costs less than a scalar `add` on the A10X (2.38 ns), then the scalar side. **Done (ospec, lspec):** add/sub/cmp/logical immediate and register (+LSL add/sub) through offset-fed gadgets with SP as a plain slot (jit/guest-arm64/alu_spec.S, "ospec"): A10X 20-37% per instruction, 0-4% on kernels (dependent chains pay the in-memory register store->load round trip, which this cannot touch). Post/pre-index and register-offset loads/stores got fast gadgets ("lspec", memory.S): they cost 2x the [x, #imm] form before; now 42-50% cheaper on the A10X, palette blit -14%. End to end on both iPads, all three on vs off: xz -9..-10%, gzip -8..-9%, python -4% (A10X). By-element multiplies and LDP/STP writeback landed in 46e558a1. **Done 2026-09-30 (SIMD memory and permutes):** offset-fed fast gadgets ("lspec") for SIMD/FP loads/stores -- [xn, #imm] and LDUR/STUR (S/D/Q), register offset (LSL/SXTW/UXTW), LDP/STP/LDNP/STNP offset form, and LD1/ST1 of one lane -- plus UZP/TRN/ZIP by arrangement in simd_spec3.S and UXTL/SXTL (USHLL/SSHLL #0) in simd_spec.S ("vspec"). A10X, fast paths off -> on: ld1 {v.s}[i] 24.2 -> 2.5 ns, ldr q 4.8 -> 2.7, ldp q 5.4 -> 3.1, ldr q reg-offset 7.2 -> 4.0, zip1/uzp1 2.9 -> 1.25, ushll #0 2.2 -> 0.96; kernels palette_blit -23%, resample -23%, mix -21%, swizzle -17% (those include the earlier lspec/vspec passes). M4 flat on the memory forms. Tests: tests/manual/arm64/vldst_lspec.c, simd_three_same.c (541 cases). **Next:** EXT (0.94% of llvmpipe; tbl with a per-imm index), XTN, MOVI/FMOV #imm, vector BIC #imm, ADDV; LD1R and multi-register LD1/ST1; then the same treatment for riscv64 (keep it close to arm64). The dependency round trip through cpu_state is now the floor for scalar code. **Done 2026-09-30 (riscv64 ALU pairs, `alu` in /proc/ish/riscv64_jit_fuse):** slli+srli/srai, slli+add (shNadd) and add+load (indexed load) run as one gadget each -- the pairs rv64gc spells where arm64 has one instruction. Per pair, off -> on: A10X add;ld 11.0 -> 3.05 ns, slli;srli 2.69 -> 1.15, slli;add 2.87 -> 1.45; M4 add;ld 1.25 -> 0.90, slli;srli 0.82 -> 0.52, slli;add 0.89 -> 0.48. A10X Devuan riscv64, best of 3: gzip -9 11.25 -> 9.81 s (-13%), python3 -9%, gcc -O2 -4%, xz -1%. **Open (A10X only):** an unfused `ld a5, 8(a5)` (destination = base) costs ~11 ns there against ~4 for `ld a4, 8(a5)`, with the gadget identical in both -- apparently a host memory-ordering replay between the two in-flight stores to the same cpu_state slot (the load's writeback and the next writer's). The M4 does not show it. It also likely affects arm64 guests' `ldr x0, [x0]`; measure pointer-chasing loops there before deciding whether a register-cached destination is worth it. **Done 2026-09-30 (riscv64 constant branches, `br`):** li t,K + b<cond> on t (either operand order) and andi t,rs,K + beqz/bnez t run as one compare-and-branch gadget. A10X, off -> on: not-taken li;beq 4.36 -> 3.04 ns and andi;bnez 4.69 -> 3.01 (now the cost of a bare branch); taken pairs flat, because a lone taken branch already costs 4.4 ns against 3.0 not taken -- the taken path (b.cond taken, then the chain dispatch) is the next thing to look at for branchy code. Devuan riscv64 workloads: gcc -1%, xz -0.5%, gzip and python3 flat (python re-measured 8 rounds; a best-of-3 +8% was noise). **Then (`btfn`):** the in-gadget jump is the cost, not the chain -- flipping the gadget layout flipped it (lone beq taken 4.4 -> 3.1 ns, not-taken 3.0 -> 4.4), and a branchless csel select was slower on both paths (5.1-5.5 ns). So each riscv64 conditional branch now has both layouts and gen picks by direction: backward branches (loops) keep the taken path straight-line. A10X, interleaved: gcc -2..3%, xz -1..2%, gzip and python3 flat, a 4-iteration counted loop -1.5%. **Done for arm64 cbz/cbnz/tbz/tbnz** (4-8% of arm64 instructions: gcc 6.4%, Doom 4.2%): offset-fed gadgets under `ospec` and the two layouts under arm64 `btfn`. A10X per body (two movs + branch): not-taken 5.21 -> 4.30 ns, taken 5.21 -> 4.74, cbnz w 5.59 -> 4.30, a 4-iteration cbnz loop 22.3 -> 19.0 (ospec) -> 18.1 (btfn). Arch arm64 workloads, btfn off -> on (medians of 4): xz -1%, python3 -1%, gcc and gzip flat. Still branchy and runtime-decoded: B.cond (bcond/bcond_nf) and the fused compare+branch gadgets (fused_cmpi/cmpr/subsi/andsi/andsr decode packed register numbers) -- offset-fed forms and _bk layouts for those are next (but compare+B.cond pairs are only ~0.1% of the arm64 gcc profile -- most b.cond follow mov or ccmp -- so this is low value). **Done 2026-10-01 (small NEON items, vspec):** MOVI/MVNI/FMOV #imm, ORR/BIC #imm, TBL/TBX (every length, 8B/16B) and the across-lanes reductions through offset-fed gadgets. A10X: movi/fmov #imm 1.78 -> 0.94 ns, addv/uaddlv/umaxv/fmaxv 2.2-3.45 -> 0.94, tbl 1-reg 2.80 -> 1.22, 2-reg 3.32 -> 1.67; bic #imm and tbx flat in a dependent chain. Multi-register LD1/ST1 left generic (0.01% of llvmpipe). What remains of llvmpipe's mix already runs on specialised gadgets; the next lever is the run translator below.
- **Bring Extreme Tux Racer back to setup-games.sh.** Taken out for 557 (2026-10-02): its race exhausted the GPU memory (fixed, MoltenVK 2cf5fc3), but two things are still wrong on the M4. The pointer is out of step with the game at the desktop's 2x UI scale (Xwayland is not scale-aware; check whether the error grows from the top-left, then try 1x), and the name boxes below. The aok-sdl-game wrapper still handles `etr` for anyone who installs it.
- **Extreme Tux Racer's name boxes draw solid on zink.** On its first menu the "player name" and "character" boxes come out filled with their outline colour and without their text on the GPU (M5 Mac and M4, through Xwayland with zink); the same build in software draws them right, and so does software Tux Racer on the GPU-backed Xwayland, so it is the client's zink rendering, not Xwayland. Racing draws correctly. Likely an SFML legacy-GL path (client-side vertex arrays with per-vertex colour, or its RenderTexture) that zink or MoltenVK mistranslates; capture the frame's pipelines with MVK_CONFIG_DEBUG=1 and compare against llvmpipe.
- **The i386 JIT is ~2.7x slower than the arm64 JIT on the same work (7-Zip benchmark, 2026-10-04).** `7z b -mmt1` (LZMA, single thread) in Alpine guests: i386 on the M5 Mac 390/367 overall MIPS, M5 Max 397/458, M4 iPad 370/370 (compress 427, decompress 312), A10X (bip) 123/122; arm64 guest on the M5 1034. Against 7-cpu.com's single-thread table the M5's i386 guest is about a 500 MHz Pentium III (P-III 1.4 GHz 1115, Atom N270 800, P4 3.0 GHz 1515, Core 2 2.0 GHz 2000) and the A10X about a Pentium MMX (125). The specialised-gadget work done for arm64/riscv64 (ospec/lspec/vspec, fused pairs, btfn) has no i386 counterpart; LZMA is memory- and branch-heavy, so profile it (ISH_JIT_PROFILE on alpine-i386-test) before choosing what to port. Re-run on the user's A20 iPhone Pro (arriving mid-October 2026; the M6 generation) -- ~/aok-bench on the M5 Max (192.168.8.123) holds the CLI and i386 root used here. **Profiled and partly done (2026-10-04):** ISH_JIT_PROFILE on 7-Zip (37.5G guest instructions) put register ALU at 34% of instructions and memory movs at 33% (base+disp, already fused; scaled index ~2%), and a host `sample` showed ~30% of the time in the load32_reg_*/store32_reg_* staging gadgets around register ALU ops -- so a reg,reg op cost three dispatches. A bigger TLB was tried and refuted (1024/4096/8192 entries: 417/425/427, 415/422/415). **Done:** fused ALU reg,reg (fused_<op>32_rr_<dst>_<src>, 448 gadgets), shl/shr/sar reg,imm and inc/dec reg, one dispatch each (`alurr`/`shift`/`incdec` in /proc/ish/i386_jit_fuse). Same binary, fusions off vs on, interleaved: M5 Max 421/420 -> 547/551, M4 iPad 355 -> 510 (+44%, 3/3), A10X 106/115/117 -> 145/151/153 (+33%, 3/3; the 448 new gadgets cost the older core nothing net). Checked by tests/manual/x86/fused_alu_regs.c (every register pair incl. ESP, identical output with the fusions off) and alu_flags. Applies to 32-bit programs on amd64 roots too (they run on the i386 engine). **Done, second round:** mov reg,imm32, movzx reg32 byte/word [base+disp], mov word [base+disp] (one gadget each) and imul reg,reg (`movimm`/`movx`/`imul`): M4 501 -> 561 (+12%, 3/3), so the M4's i386 7-Zip went 355 -> 561 in a day; M5 Max loaded and noisy (4/5 rounds up). A10X 151 -> 165 (+9%, 3/3). **Done, third round:** [base+index*scale+disp] addresses in one gadget (`addrsi`, 256 gadgets, scale baked in; every scaled-index operand paid addr_<base> + si_<index>) and cmp/test reg,reg + jcc in one (`cmprr`: cmp with c/z/cz/s/sxo/sxoz, test with z; gen_try_fuse_jcc rewrites [load32_reg_<dst>][fused_cmp32_<cond>_reg_<src>] after checking the load is the CMP's own via x86_fuse_start): M4 560 -> 595 (+6%, 3/3), A10X 152 -> 164 (+8%, 3/3) -- M4 i386 7-Zip 355 -> 595 (+68%) over the day. **call/ret is structural, left:** an empty call+ret is 4.7 ns on the M4 (plain loop iteration 2.1 ns); it is two block transitions, each paying chain_ip (poke check through cpu->poked_ptr -- required, the JIT runs on frame->cpu, a copy -- plus the chain-budget load/decrement/store and the last_block store). Cutting that needs a free host register for the budget or a different poke protocol; both touch the lost-poke history (c5692489), so measure-first territory. **Done, 2026-10-05 (Python, not 7-Zip):** a host profile of Alpine i386 Python put ~15% of the time in vec_helper C calls and ~7% in leaving the JIT for indirect jumps. Alpine's i386 gcc moves 64-bit values through SSE (movd r32 -> xmm, punpckldq, movq xmm -> m64; movsd for doubles), ~7% of Python's instructions, each a C call with every register spilled and reloaded around it -- the Devuan profile above has almost no SSE, which is why this was missed. Those moves are now gadgets (misc.S vec_movd_*, vec_ld*/vec_st*, vec_copy64z, vec_punpckldq; the 64-bit memory forms keep the helpers' dmb ish). And jmp/call through a register or memory (Python's computed-goto dispatch) enter the target straight from the dispatch loop's per-thread block cache (jit_frame.i386_cache, control.S i386_indir_chain) instead of returning to C every time. M4, Alpine 3.24.2 i386, app builds alternated 3/3: Python fib/method/dict/str 166/735/589/615 -> 118/585/517/456 ms, jit_bench ind 1830 -> 744 ms, 7-Zip flat (~595). Checked by tests/manual/x86/i386_sse_moves.c (random SSE-move runs, identical output to the C-helper build) and the i386 and amd64 suites. Then pshufd (register form), pxor, paddq and psrlq/psllq by an immediate the same way: Python dict 515 -> 454, str 466 -> 445 ms (M4, 3/3). **Measured, not done (2026-10-05):** i386 7-Zip's host profile is flat now; the visible remainder is ALU reg,[mem] (`addl d(%ebp), %eax`, `xorl d(%ecx,%ebx,4), %eax`) still staged through load32_reg/addr/op_mem/store32_reg, but it is ~3% of 7-Zip's instructions, so fusing it per (op, reg, base) is worth ~1-2%; the rest of the gap to amd64 (604 vs 812) is i386 code being register-starved (memory operands on the stack frame). **Tried and dropped:** no dmb ishld in the i386 branch gadgets that load no guest memory (jmp, jcc, call, loop, register-form fused jcc), as amd64's gret_nomem: flat on the M4 (3/3) and the A10X.
- **Host an MCP server in the app (Discord suggestion, Rogue, 2026-10-03).** "You should add an mcp server hosted to the ish-aok app. That way we can connect an ai agent and control ish-aok from an external agent." AOK already has the client side (LLM Chat takes MCP servers as tool sources, 12e7f90be); this is the reverse. Shape to decide: (a) app-hosted, Streamable HTTP on a port, so it can reach app-level things a guest cannot -- Workspace (open applets, /proc/ish/workspace), settings (/proc/ish/defaults), roots (list/boot), terminals (send input, read the screen), checkpoints -- alongside guest commands and files; or (b) a guest-side stdio server reached over ssh (`ssh ipad aok-mcp`), simpler and already possible with sshd, but guest-only. Either way: off by default, a per-device token, bound to loopback/Tailscale unless the user widens it, every tool call logged, and a confirmation mode for destructive tools. Start with (b) plus a few app tools through /proc/ish, then decide whether (a) is worth an in-app HTTP server.
- **Find out what "AOJIT" is in ish-arm64 / "open minis" (Discord, cacophonousStrife, 2026-10-03).** A user asked about "the new AOJIT thing introduced in ish-arm64/open minis" and could not find its release notes. Look it up (the ish-arm64 fork's repository and releases), say what it is in a reply if it helps the user, and note anything worth borrowing for AOK's JIT (compare with the run-translator and gadget-body items above).
- **An arm64 desktop on any root: prototyped, measured, not worth it for speed (2026-10-03).** Mechanism that works: graft an installed arm64 root into the booted one with two symlinks -- `/usr/lib/aarch64-linux-gnu -> /AOK/roots/Devuan6-arm64/usr/lib/aarch64-linux-gnu` and `/usr/lib/ld-linux-aarch64.so.1 -> aarch64-linux-gnu/ld-linux-aarch64.so.1` -- and arm64 programs exec normally (no foreign_exec, whose ROOT mode would put the compositor's Wayland socket in the other root's home), with every compiled-in arm64 path (Mesa drivers, GBM, Wayfire plugins) resolving and no environment for native children to inherit; /usr/share data comes from the booted root. On bip's Devuan riscv64 root the arm64 labwc ran as the desktop on the GPU and the bundled aarch64 wayvnc ran too. Measured (bip, A10X; labwc CPU per displayed es2gears frame; 2 interleaved rounds): GPU native riscv64 4.99/4.68 ms vs arm64 4.70/4.33 ms (~7%); software native 579/546 ms vs arm64 605/572 ms (arm64 ~5% dearer). The compositor's cost is not guest-instruction bound, so an arm64 desktop buys little; revisit only for software GL clients (llvmpipe: NEON on arm64, plain C on rv64gc) or if a profile shows otherwise. Graft removed from bip afterwards.
- **Software compositing at UI scale 2: fixed (2026-10-04).** bip (A10X), labwc in software, es2gears, 2732x1932 at scale 2: 1.6 fps and ~580 ms of compositor CPU per frame, now 19.8 fps and 10.7 ms. Two causes, both found with ISH_PIXMAN_STATS on labwc: es2gears' EGL picked 10-bit buffers, which pixman converts in floating point (start-wayland.sh exports allow_rgb10_configs=false: 8.7 fps, 75 ms), and the 2x bilinear stretch of every scale-1 client ran in pixman's C because the accelerator declined transforms (now ISH_PIX_OP_SCALE_BILINEAR on the host, byte-exact with pixman on riscv64 and aarch64; pixman_accel_plan.md). es2gears' own software rendering is now the limit. x86 guests done too (2026-10-04): pixman's SSE2/SSSE3 paths follow NEON's rule exactly; shim test PASS on Devuan x86_64, Alpine x86_64 and i386 with ~760 stretches accelerated. Left: OVER from x8r8g8b8 on aarch64/x86 (needs pixman's cover/opaque decision mirrored; pixman_accel_plan.md NEXT 0).
- **Try the GPU on a Devuan i386 root (2026-10-03).** Debian 13's i386 Mesa ships the Venus driver (packages.debian.org filelist: libvulkan_virtio.so), and setup-gpu.sh now lets Devuan riscv64 and i386 through (riscv64 verified on bip: Venus on the A10X, zink GL 2.1/GLES 2.0, labwc composited natively with the bundled riscv64 wayvnc detached by wl-present 2/2). i386 is untested: there is no Devuan i386 root here or on the iPads, and a 32-bit guest's virtgpu ioctls (u64 fields, mmap of blobs) have never been exercised. Make a Devuan i386 root, run setup-gpu.sh, vulkaninfo --summary, then a desktop session.
- **The arm64 Wayfire and wf-panel on a riscv64/x86 Devuan root get the root's shims (2026-10-03).** start-wayland.sh exports LD_PRELOAD with /AOK/bundled/glibc-<root arch>/libish-{pixman,wl-release-guard}.so; an arm64 compositor or panel (Wayfire everywhere, setup-wayfire.sh) cannot load them, so ld.so prints two "cannot be preloaded" errors at each start and they run without the pixman accelerator and the release guard. wayvnc is fixed (wl_preload_for gives it its own architecture's shims), but the compositor and panel cannot simply be given arm64 shims: the programs they launch inherit LD_PRELOAD and are native. Idea to check first: glibc expands `$LIB` in LD_PRELOAD per process (Debian: lib/<triplet>), so shims served at /AOK/bundled/glibc/lib/<triplet>/ and LD_PRELOAD=/AOK/bundled/glibc/$LIB/libish-pixman.so would give every process its own -- verify the expansion on Devuan arm64/riscv64/amd64 and on Alpine (musl does not expand it; musl roots have no foreign-arch compositor anyway).
- **Doom games: start with no sound on fewer than four CPU cores (user request, 2026-10-03).** Chocolate Doom, Crispy Doom and Freedoom as setup-games.sh installs them should open with sound off (`-nosound`, or the launcher's equivalent) when the guest has fewer than four processor cores -- the A10X class, where mixing and the sound pipeline cost more than they are worth. Count the cores the way a program sees them (`nproc`, the cpuset, not cpuinfo's online count -- [[guest-cpu-count-cpuset-shape]]), decide in the launcher wrapper so the menu entries and a shell both get it, and leave a way to force sound on.
- **Chocolate Doom still fails to start on bip with the current build (user report, 2026-10-03).** After the "No connection" / launch fixes of 2026-10-03 it worked on the M4 and the user's rebuilt app, but on bip (A10X, iPad Pro 12.9 2nd gen) it still does not start: ktop shows chocolate-doom running and using a whole CPU, and nothing appears on the display -- busy, not crashed, and no window mapped. Next: sample where it spins (guest PC sampler, ISH_GUEST_PROFILE, or a host backtrace of its thread) -- on bip, read dmesg first, then run the menu entry's exact command in a terminal in the Wayland session and capture its stderr; compare with the M4 (same root type? GPU vs software -- bip's zink/Venus path differs, see the #484 A10X entry above), and check whether sound start-up (the item above) is what hangs it.
- **Games scouting follow-ups (2026-09-28, Devuan arm64, M5 Mac).** Run fine: ScummVM freeware (Flight of the Amazon Queen, Lure of the Temptress, Drascula -- the last asks for ripped CD audio), Crispy Doom, OpenTTD (with opengfx/opensfx/openmsx), SuperTux (zink's GL 2.1 fails its GL check, it falls back to SDL drawing), Pingus, LBreakoutHD, LTris 2, Powermanga, C-Dogs SDL, openMSX (C-BIOS, needs cartridges), DOSBox, and on the GPU after 524eac6a/7f9f01f8 Neverball, Armagetron Advanced, Teeworlds, Chromium B.S.U., SDL-Ball, LinCity-NG. Open: X-Moto exits at once with "Unable to get xmDb version" in software and on the GPU (its sqlite database -- a guest file-locking or mmap gap?); Frozen Bubble and Rocks'n'Diamonds sit on their loading screens past 45 s (measure whether it is slow or stuck); `apt-get install emutos` failed (Hatari has no TOS without it); dsda-doom's `-timedemo demo1` cannot find the lump. Then add the good ones to setup-games.sh as an optional second set.
- **motepad (the native editor): close the gaps its own page lists.** `/AOK/docs/motepad.md` ("What it does not do") and `kernel/native_motepad.c` agree, checked 2026-09-28: no undo (Ctrl-Q twice is the only way back), no selection or clipboard (Ctrl-K deletes a line with nowhere to paste it back from), and no wcwidth (a double-width CJK glyph or emoji counts as one column, so the drawn cursor can sit a cell off the edit). Add an undo stack, a cut/copy/paste buffer that also reaches the iOS pasteboard where the app hosts the session, and wcwidth-aware column arithmetic; then update the page's list. The Workspace MotePad applet is a separate front end and was not reviewed here.
- **x86_64-host builds: the amd64 interpreter lacks this cycle's x86 fixes, and x86_fp_env fails.** On an x86_64 Linux host (GCC build, as CI does; recipe below) the amd64 engine is the pure interpreter -- the amd64 JIT gadgets are aarch64-only -- and the 2026-09-30 fixes that live in jit/gen.c's amd64 arms are not there: priv_gp (ring-0 0F opcodes SIGILL instead of #GP), movnt_stores (MOVNTPS/MOVNTPD/MOVNTI SIGILL), sse_align_gp (misaligned MOVAPS etc. run silently) all fail under amd64 on that build. The i386 engine passes them (its x86_64 gadgets got vec_align16). x86_fp_env fails there too, on both engines: ucomisd qnan (ZF/PF 0x1 vs 0), divsd FTZ (0x5555555555555 vs 0), and on amd64 cvtsd2si 1e300 flags (0 vs 0x1). Fix in emu/amd64_interp.c (decode the same cases) and in the x86_64 SSE gadgets. Recipe (camd): rsync the tree minus /build*, /.git, /deps, /app (then add app/*.h and docs), /.claude, /e2e_out, /subprojects; `ln -s ~/ish-AOK/deps deps`; `meson setup build-x64 -Dgpu=disabled`; touch changed files after an rsync (the Mac's mtimes can be older than camd's objects); roots via `rsync -aS --exclude data/tmp/*` (the amd64 root's /tmp holds two 5 GiB sparse files).
- **Make meminfo Shmem/AnonPages/Mapped Linux-shaped -- DEFERRED by the user 2026-10-09** (it is not small). AOK counts page-table entries per address space at mmap time; Linux counts resident pages once each from first touch (the measured table is under "`/proc/meminfo`'s Shmem, AnonPages and Mapped are not Linux's figures" below). What it takes, sized 2026-10-09: (1) move the class counters from entry publish/clear (mem_entries_published/mem_entry_cleared, emu/memory.c) to first touch/untouch (mem_pt_touch/mem_pt_untouch), which fixes the untouched rows; (2) count a frame once across address spaces -- a per-page touched-mapcount on struct data for anonymous pages shared after fork (pt_copy_on_write copies PT_TOUCHED, so a separate bit or count is needed: RSS must keep counting the child), and a global table keyed by mem_shared_page_id for file/memfd/shared-anon pages, since two mappings of one file are two struct datas; (3) Shmem from contents, not mappings: tmpfs and memfd allocated pages whether mapped or not (tmpfs keeps no such counter; memfd is an unlinked host file), plus touched shared-anon and SysV pages. Every site that moves an entry has to keep these right -- unmap, COW break, mremap (pt_move), swap-out/in, eviction -- so the risk is in the memory core. Update tests/manual/meminfo_scaling.c to Linux's table, and keep the read a counter load (ISH_MEM_CLASS_CHECK's walk becomes a frame walk).
- **Done 2026-10-09: O_PATH through a /proc link to a pipe, socket, anonymous inode or removed directory** opens, as on Linux: an opath_held handle (fs/generic.c) holds the description, forwards fstat and getpath, reads EBADF, and reopens what it holds through its own link; a socket opens through /proc as ENXIO. tests/manual/opath_proc_nameless.c (26 checks, camd and AOK). Left: a non-O_PATH open of a pipe through /proc opens only the direction the descriptor already has (AOK's pipes are host pipes, and a read end holds no write end), and an open through the handle's link hands back the held description rather than a new one (its offset and flags are shared).
- **Done 2026-10-01: F_GETFL reports what Linux reports.** realfs_getflags takes the guest-owned bits (O_APPEND, O_NONBLOCK, O_DIRECTORY, O_NOFOLLOW, O_LARGEFILE) from fd->flags and only the rest from the host; every open by a 64-bit task gets O_LARGEFILE (force_o_largefile; not O_PATH); fd_getflags drops O_CREAT/O_EXCL/O_NOCTTY/O_TRUNC/O_CLOEXEC; a by-path reopen stores the caller's flags, not its internal O_NOFOLLOW. Measured against Linux 6.12 x86_64 and -m32 first: 64-bit opens report O_LARGEFILE always, 32-bit only when asked (musl always asks). This also fixes the pipe O_NONBLOCK lie below. Test fcntl_getfl_flags (passes on Linux 64/32-bit).
- **Done 2026-10-09: a /proc fd link to a target outside the caller's chroot leads there**, as Linux's nd_jump_link does: __path_normalize (fs/path.c) walks a magic link whose text is "(unreachable)..." from the real root with the marker dropped, so `/proc/self/fd/N`, `/proc/self/fd/N/below` and `/dev/fd/3/script` reach the file opened before the chroot, not the decoy at the same path inside. readlink still shows the global path. tests/manual/proc_link_chroot_escape.c (camd `unshare -rm`; the build before fails 2 checks).
- **Done 2026-10-01: Alpine 3.24.2 promoted to official** (ish-AOK-rootfs b937aab; the app picks the catalogue up within its 10-minute refresh). Gate: the full suite on each 3.24.2 root (i386, x86_64, aarch64, riscv64) plus Devuan 6 arm64 (glibc), on the Mac CLI and on the M4 with each root BOOTED -- all pass; Mac-only inaddr_any_iface (link-local interfaces), and one i386 x86_unaligned_lock device run that passed alone (queued above). pixman_accel/pixman_shim pass on all ten legs once `pixman` is installed (CLI: ISH_PIX_ACCEL=1). The Mac test roots alpine-{i386,amd64,riscv64}-test are now 3.24.2 (old ones kept with their version suffix) and alpine-arm64-324 is a clean 3.24.2 arm64; alpine-arm64-test tracks edge (3.25 alpha). The M4's four 3.24.2 roots carry the toolchain and sshd on 1022 for booted legs; the compiled-test cache store was seeded from the Mac roots and pushed (2018 entries), which took the device legs from hours to minutes.
- **Done 2026-10-01: pwritev2 honours its flags.** RWF_NOAPPEND writes at the offset on an O_APPEND description and RWF_APPEND appends (fd->pwrite_append, read by every fs pwrite through fd_pwrite_appends, FUSE included), RWF_DSYNC/SYNC sync after the write, RWF_NOWAIT/ATOMIC/unknown are EOPNOTSUPP; preadv2 refuses unknown bits. Two bugs: the flags were ignored, and the arm64/riscv64 entry read them from pos_h (always 0) instead of the sixth argument. With musl 1.2.6 (Alpine 3.24) a plain pwrite() on an O_APPEND file went to the end of the file instead of its offset. Test pwritev2_flags (matches Linux 6.12, 64- and 32-bit).
- **Done 2026-10-01: clone() and unshare() agree on namespaces AOK lacks** -- both ENOSYS for CLONE_NEWNS/NEWPID/NEWNET/NEWUSER/NEWCGROUP (clone said EPERM). Linux built without a namespace type says EINVAL; ENOSYS is kept on purpose so `unshare -n` says "Function not implemented". Test clone_unshare_agree (root; passes on Linux, where both succeed); book ch41.1 updated.
- **Done 2026-10-09: FUSE through fsopen() mounts.** sys_fsmount_guest (fs/mount.c) no longer opens a FUSE mount's root, a request to a daemon that mounts first and serves after: it hands back an opath_link handle naming the root, which is all move_mount wants. tests/manual/fuse_fsopen.c times fsmount with no daemon serving, then serves and stats/looks up through the mount (camd 64- and 32-bit; the build before hangs until the alarm). fs/fuse.c's header and book ch20/ch41.6 updated.
- **Done 2026-10-01: book ch07 brought up to date** with the GNU `as` bypass removal (23edd81e), the interpreter's size (about 18,000 lines) and where the amd64 JIT runs (any aarch64 host, the Mac CLI included).
- **Workspace LLM Chat toward OpenCode: what the phases left.** Chosen 2026-09-28 over embedding OpenCode natively. Done: phase 0 (the chat's own files, app/LLMChat*.m), phase 1 (read_file/write_file/edit_file/list_directory/glob/grep; allow/ask/deny per category plus shell rules, Settings -> LLM Client -> Tool Permissions; tests/unit/llm_permissions_test.m) and phase 2 (per-chat working directory in the chat menu, saved as the session's "workingDirectory"; the nearest AGENTS.md or CLAUDE.md sent with every tool request; todo_write; /compact and "Summarize Chat", automatic above 75% of a known context window; the empty chat names what the tools can do). Both phases were run end to end in the simulator against scripted OpenAI-compatible servers. Not yet exercised: the file tools under "Open Everything as Default User" (the mode-bit check in ISHLLMAccessProblem), the Working Directory menu item itself (injected taps do not open UIMenu buttons; the saved-setting path it writes was tested), and automatic compaction (it shares /compact's code). Apple Foundation Models still offers run_shell alone (4K context). Phase 3 done so far: API keys in the Keychain (cc0ca8a0; they were readable by any guest process under /proc/ish/defaults), change review with diffs and /undo (41b9c831), Anthropic's Messages API with prompt caching and refusal fallbacks (4abf8524; tested against an Anthropic-format mock, not yet the real API). Streaming inside the tool loop, both formats, with Stop (73196224). MCP servers as tool sources (2026-09-29): remote Streamable HTTP servers (bearer token in the Keychain) and stdio servers run in the guest as the tool account (kernel guest_process_spawn_user, 47ab754f); Settings -> LLM Client -> MCP Servers, or /mcp; calls go through the "MCP Tools" permission category; tests/unit/llm_mcp_test.m. Run end to end in the simulator against a mock remote server (session id, protocol header, paginated tools/list, SSE replies, isError, structuredContent, DELETE on disconnect) and a BusyBox sh stdio server in the guest (a ping from the server answered). Not yet exercised: a real npx/uvx server (first start downloads it: initialize waits 180 s), the real Anthropic API, and MCP resources, prompts and sampling (declined with -32601; only tools are used). A server that fails is not retried for five minutes unless its settings change or a Check succeeds. Background agents (2026-09-30): each chat is an agent (app/LLMChatAgent.m) that keeps working when another chat is shown or the window closes, on its own destination (set per thread for its requests) and tool queue; approvals wait until that chat is opened; spawn_agent/agent_result start sub-agents one level deep, at most four working per chat; a status panel shows phase, elapsed time, round, tool calls, context use and other chats; /new, /chats, /agents; prompts sent mid-reply queue. Run in the simulator against two mock servers; not yet exercised: the Anthropic, Gemini and Apple Foundation Models paths after the move (ported unchanged), and a sub-agent whose parent chat is reloaded after a relaunch.
- **OpenCode in an arm64 guest: what the hang hunt left.** Not a kernel hang. On 2026-09-28 four `opencode serve` runs (1.18.33, `build/alpine-arm64-test`) did about 30 agent turns with the bash and write tools, three of them concurrently, and three `/etc/*` permission asks left pending for up to 37 minutes before a reply; every turn finished and the server kept answering. An in-guest busybox `wget` of a fresh `/doc` took 31-77 s against host curl's 26-35 s, not 937 s. The reported wedge came while this Mac was thrashing (18 GB of swap in use, a JetsamEvent, load 100-300, background CLIs at nice 5), and AOK made it worse: Bun's 64 GiB JavaScriptCore reservation was materialised whole by one mprotect, 1.1 GB of page tables built with every guest thread stopped (22dbee6d: VmPTE 1.08 GB -> 18 MB, server footprint 2.2 GB -> 1.1 GB). Left: `pt_copy_on_write` still materialises every reservation of a process that forks (read, not measured); a refused split (MEM_LAZY_SPLIT_LIMIT) still costs the smaller side; cold start is 60-150 s and a turn 13-60 s on this loaded Mac.
- **Misaligned LOCK starves the process's other threads: diagnosed 2026-10-09, fix measured and not kept.** Reproduces reliably on the M4 and on the Mac under `setsid` (6 runs in 30): host samples (Mac `sample`; the M4 through `xctrace record --template 'Time Profiler' --attach iSH-AOK`, which does not kill the app as lldb does) showed the other thread blocked in read_lock at the top of task_run_current -- a new thread got a block or two of glibc start-up between split ops, 1.5 s with not one store -- while the splitter waited in x86_lock_split_begin for it to leave the JIT: each split op re-queues as a writer a few instructions after the last, and readers yield to a queued writer. (Admitting queued readers before an op does nothing: the reader gets in and is poked straight back out.) A quantum fixes it -- after an op that found siblings in or near guest code (task_poke_shared_mem counting them), the next leaves the JIT with a yield interrupt and waits out 100 us with nothing held: Mac setsid 6/30 -> 0/30, M4 ~50% -> 0/20 -- but when every thread is a splitter it is ruinous: amd64_xchg_mem (four threads, misaligned xchg) went from 1-6 s to over ten minutes waiting every op (and livelocked when the wait was a sleep inside the JIT holding the read side, which held off the retries' mem_ptr_fault write_lock), and to 166-350 s waiting every eighth. The heavy part under contention is the retry itself (a lost trylock of x86_split_lock goes out as INT_PF, through mem_ptr_fault's blocking write_lock); a fix needs a cheaper hand-off between splitters before a quantum for non-splitters can be afforded. Tried the same day: the JIT's retries leaving through a plain yield exit (run loop: sched_yield, re-run) instead of the page-fault round trip -- worse, amd64_xchg_mem 28-47 s against the current 1-26 s (the losers re-try hot and keep poking the winner; mem_ptr_fault's write_lock at least parks them). Note the baseline's own spread. Split locks are slow and unfair under contention by construction here (they need the address space exclusively), much as on hardware, which takes a bus lock for them -- Linux 5.19 and later even penalise a user-mode split lock with a 10 ms sleep (split_lock_mitigate); real code avoids them. Left as is.
- **i386 misaligned LOCK: starvation across a page (found 2026-10-01 by the 3.24.2 device leg).** tests/manual/x86_unaligned_lock's interlock phase (a `lock incw` thread against a plain-store thread) on the i386 root. `word_at_15` -- crossing a page, so x86_atomic_split -- can starve the other thread: the M4 leg saw 0 stores against 6605 increments in 1.5 s (passes alone). mem->lock is writer-preferring, and each split op re-queues as a writer a few instructions after downgrading, before the poked sibling can take the read side back. A fix that admits queued readers before each upgrade (read_lock_admit_waiting; patch kept at the session scratch, rw_locks.h/tlb.c) removed the starvation but cut that case's throughput ~100x (thousands of ops per run to tens), so it was not committed; a cheaper hand-off is needed. The "rare lost store" at `word_at_1` reported alongside it was the test, not AOK (fixed 2026-10-01): every lost value it logged was the 16-bit counter wrapping past 0xffff while the storer was descheduled for a few ms (marker 0x8700 read 0x697), where a lost store reads the previous marker; the check now counts only values within one step below the marker, and the fixed test passes 10/10 on the AOK i386 root and on camd (64-bit and a static i386 build). The earlier camd losses at word_at_15 (4 of 8.8M) were most likely the same wrap. Seen once more 2026-10-07 at word_at_1 (1 of 237,036, i386 root, Mac load average ~30 during a two-suite run; passes alone): a wrap can still land one step below the marker by chance, so if it recurs, widen the counter or log the read value to tell a wrap from a loss. Also: a zig/clang-built i386 copy of the test fails every qword case on both AOK and Linux (its 64-bit path differs from gcc's); build it with the guest gcc. **amd64 too (557 device leg, M4, Alpine 3.23.3 x86_64 booted, 2026-10-02):** `qword_at_12: a thread never ran (10722 increments, 0 stores)` in the full suite, then 1 of 6 alone (`dword_at_14`, 6767 increments, 0 stores) -- not page-crossing cases, so the hand-off problem is not only x86_atomic_split's; the Mac legs passed it on all five roots. **Refuted 2026-10-09:** admitting queued readers every 16th split op (a readers_waiting count in wrlock_t, read_lock_admit_waiting) changed nothing -- the starved thread is not a reader queued on mem->lock -- and cost the incrementer 10x and the storer 4x on a word_at_15 probe; reverted. Seen on the Mac too now (x86_unaligned_lock at load 30-90: 1-3 in 10 runs on builds before and after x86_lock_rmw, at dword_at_14, qword_at_12, word_at_15, qword_across_page; a standalone word_at_15 probe never starved in 150 rounds). Next: catch it with a host `sample` while the storer is at 0 stores, to see where it waits (300 runs, six in parallel, did not reproduce it on 2026-10-09). **The word_at_1 "lost store" settled 2026-10-09:** the same parallel load gave 9 in 150 runs on the build before x86_lock_rmw and 15 on the build after -- the wrap, not AOK. The storer now counts a read only while fewer than 65536 - 2 steps of increments have landed since its store (x86_unaligned_lock.c); 90 runs then gave none, and a copy with the LOCK prefix removed still loses thousands natively on camd.
- **i386 host SIGSEGV in concurrent_exec_tlb under load (found 2026-10-01) -- see "Stale TLB after exec" above: not reproducing.** The i386 root's `concurrent_exec_tlb` (8-way fork+exec of cc1, 32 rounds) killed the CLI with EXC_BAD_ACCESS in `fpu_ldm80 <- back_fhelper_read80 <- cpu_run_engine_to_interrupt`, always at a page-aligned host address (0x107758000, 0x104d24000, 0x11bfb8000). Not the cross-page check (read_prep sends offsets past 0x1000-10 to crosspage_load), so an x87 80-bit load went through a TLB entry whose host page was gone -- the #469 stale-TLB-after-exec class that test was written for, which was fixed for the arm64/riscv64 frontends. Repro: four copies at once of `bin/concurrent_exec_tlb` in build/alpine-i386-test, ~1 run in 16 dies; alone, 12/12 pass. Independent of fork inheritance (one crash each with ISH_JIT_INHERIT=0 and =1). Next: check how the i386 frontend revalidates its TLB after exec against the arm64 fix. (The fhelper path that crashed is gone since 2026-10-06 -- x87 m80 loads are x87.S gadgets through read_prep -- so re-run the repro; the stale entry, if it is that, would now show in an ordinary load.)
- **Crash triage 2026-10-01 (Organizer download, 104 logs, 14 points, newest 2026-09-26; none from 556 yet).** Open: RUNNINGBOARD 0xdead10cc (iOS killed the suspended app holding the fakefs SQLite lock), 22 logs over 549-555, 9 of them on 555 -- 4 inside -[AppDelegate performDnsRefresh:] (fakefs_open, fd_close -> fakefs_inode_orphaned) and 5 in guest file syscalls (openat, fchownat, fsetattr, symlinkat, exit_group -> fakefs_inode_orphaned) racing suspension. Both are what 6de50726 (first in 556) targets: the DNS refresh holds its own background assertion and the quiesce gate no longer lapses. Next download: confirm no 0xdead10cc on 556; if any remain, classify the lock holder the same way (python over each log's threads for fakefs_/db_/sqlite frames). Fixed in shipped builds: checkpoint save SIGBUS on a past-EOF file page (555, iOS 27 beta, one iPhone 12; 1447dab9 in 556); macOS FileProvider __FILEPROVIDER_BAD_EXTENSION__ (548-554, one Mac on macOS 27 beta; c94e0054 in 555, af01f729 in 556); iOS 15 setitimer abort #595 (553; de847adc in 555, issue closed). The rest are 548 and older (tmpfs_write assert, FileProvider childItemCount/loadToURL, realfs_telldir, jit_crash_bus_fn on 538-542), triaged in earlier releases. **Refreshed 2026-10-02 (557 pass): 6 new logs, still none from 556.** Five are FileProvider RUNNINGBOARD 0xdead10cc on build 530 (one iPhone14,6, iOS 27.0/27.0.1, 2026-09-27..10-01) inside sqlite3WalClose from -[ISHFileProviderMount dealloc] -- the suspension-path close that the idle close (after 546) moved off it; that device is simply still on 530. One is an app 0xdead10cc on 555 (iPhone13,2, iOS 26.7), the class 6de50726 targets in 556. Nothing for 557.
- **Codex CLI cannot run shell commands: bubblewrap needs user/mount namespaces (TestFlight feedback on 556, 2026-09-28).** "Codex CLI works until it tries to run a shell command" (iPad Pro M2, iOS 27). Codex 0.159 runs every command through its Linux sandbox, now bubblewrap (bundled bwrap; Landlock is only a hidden legacy fallback, which AOK also lacks), and has no fallback when bwrap cannot start. Reproduced on the Mac with `codex sandbox -- /bin/echo x` in build/alpine-arm64-test (npm i -g @openai/codex): first "bwrap: Can't read /proc/sys/kernel/overflowuid", and with those sysctls added (65534, as Linux) "bwrap: Creating new namespace failed: Function not implemented". Shipped for now: /AOK/fixes/codex/README.txt and fix-codex.sh, which puts `sandbox_mode = "danger-full-access"` at the top of ~/.codex/config.toml (codex doctor then reports unrestricted fs, approval OnRequest unchanged); `codex --sandbox danger-full-access` for one session. Note `codex sandbox` ignores sandbox_mode from config.toml by design (debug_sandbox.rs), so test the setting with codex doctor. Real fix, a project: enough CLONE_NEWUSER + CLONE_NEWNS (uid_map/gid_map/setgroups, per-namespace mount tables, tmpfs/bind/remount ro, pivot_root, a fresh /proc) and CLONE_NEWNET (loopback only; Codex unshares the network when it is disabled) for bwrap 0.11 to start -- also what Flatpak and other bwrap users need. Check bwrap's own test suite as the oracle.
- **#625: "Browse Files…" in Filesystems aborts the app (iPhone18,5, iOS 27.0.1, build 556; found in the 557 pass, 2026-10-02).** The reporter's diagnostics show five SIGABRT MetricKit payloads (exceptionType 10) in 25 minutes, with no stack. It does not reproduce in the iOS 27.0 simulator: on an iPhone 18 Pro sim, Import -> Browse Files… (the real UIAlertAction, fired with `_dismissWithAction:` from lldb on the root-selection RootsTableViewController) presents the UIDocumentPickerViewController (`initWithDocumentTypes:inMode:Import`, tar/gzip/bzip2) and the app stays up; so does presenting it directly. Untried: the Filesystems screen reached from Settings or a Workspace window in a booted session, and a real iOS 27.0.1 device. Next: the MetricKit payload's callStackTree (the app keeps the JSON; ask the reporter for it, or for the `.ips` from Analytics Data) or a 556 crash in the Organizer; then move the picker to `initForOpeningContentTypes:asCopy:` if the deprecated initializer is implicated.
- **riscv64_singlestep failed once under five-root load (557 Mac gate, 2026-10-02).** "counter ran ahead" (0x289f7 at the first step, limit 0xa0) and only one step, at load ~150-250; 9 of 9 runs alone pass on the same binary. Likely a stop-ordering race in the test, not the kernel: if the child's `raise(SIGSTOP)` lands before `PTRACE_SEIZE`, the parent can see the plain stop first and answer it with `PTRACE_CONT(SIGSTOP)`, which from a trap stop just resumes the loop until the next stop is taken as the first step. Check against camd (Linux) with a delay injected before the SEIZE; if Linux shows it too, synchronise the child with a pipe before it raises SIGSTOP.
- **timer_thread_cpu_early fails on the A9 (ip5, iPadOS 16; 557 device leg, 2026-10-02).** 29-30 of 30 rounds, alone and in the suite: a CLOCK_THREAD_CPUTIME_ID timer armed for 5.0 ms of CPU fires with 0.5-4.8 ms used. Passes on the M4, the Mac and all five Mac roots. task_thread_cpu_time_ns (thread_info) and the timer path have not changed since a388da04 (556), and 556 was never run on ip5, so this is not known to be new in 557. Suspect the two clocks disagree on that SoC/OS: the timer's thread_info reading against whatever the guest's CPU-time read uses. Next: on ip5, print both for one thread side by side over a sleep-and-burst loop.
- **wayvnc 0.9.1 dies when wl-present detaches it (ip5 2026-10-02, M4 2026-10-03) -- mitigated; the cure is wayvnc 0.10.** Not AOK: wayvnc 0.9.1 frees its buffer pool on detach while neatvnc still holds a frame from it, and the frame's release then writes into the freed pool (`malloc_consolidate(): unaligned fastbin chunk detected` on the M4, a NULL-based store in libwayland-client on ip5). A resize just before the detach -- every session start, as wl-present sizes the output and then detaches -- makes it likely. Reproduced on the M4 with a streaming RFB client, a SetDesktopSize, and `wayvncctl detach` 0-120 ms later: 0.9.1 died 5 of 15 interleaved rounds, wayvnc 0.10.2 (neatvnc 1.0.2, aml 1.0.0) 0 of 35. Upstream fixed it in 0.10.0 (2897d15 "Unlink destroyed pool from inflight buffers", 5d7784d "Make wl_buffer inert on detach"). Done: start-wayland.sh restarts a wayvnc that dies (up to 5 times; the app's VNC retry window is 60 s), keeps a detached marker so a restart during wl-present stays detached, and starts a 0.10+ wayvnc with `--detached` then attaches it (0.10 exits on detach otherwise); `fix-neatvnc --version 1.0.2` builds the 0.10.2 chain (jansson added to its deps). Verified on the M4 with the app (2026-10-03): the build's start-wayland.sh writes the marker and keeps wayvnc 0.9.1 alive across 5 launches; after the tool, 4 launches with wayvnc 0.10.2 started `--detached`, attached, detached by wl-present, still up, app connected. **Then (2026-10-03, user's call): Devuan uses a bundled wayvnc.** /AOK/bundled/devuan6-{aarch64,x86_64} carry wayvnc 0.10.2 + neatvnc 1.0.2 + aml 1.0.0 (opt/AOK/tools/wayland/build-wayvnc.sh, tools/build-bundled.sh targets wayvnc-aarch64/wayvnc-x86_64; RUNPATH $ORIGIN, no TLS/PAM/H.264); start-wayland.sh links them into the session bin when `wayvnc -V` runs (else Devuan's, with the restart safety net); setup-wayland.sh installs wayvnc.depends; setup-wayfire.sh adds them as :arm64 on i386/riscv64. M4, /usr/local cleaned to Devuan's 0.9.1 only: 5 of 5 launches ran /AOK/bundled/devuan6-aarch64/wayvnc --detached, detached, alive, app connected. Alpine 3.24 ships 0.10.0 and needs nothing. HEAD (39df416a9) on devices: M4 Devuan arm64 2/2 and bip (A10X) Devuan arm64 Wayfire 4/4 on the bundled aarch64 wayvnc, detached by wl-present, alive, app connected; bip Devuan riscv64 (labwc in software, after setup-wayland.sh installed wayvnc.depends) 3/3 on the bundled riscv64 wayvnc, attached to HEADLESS-1, app connected. **557 users:** Devuan GPU desktops can lose wayvnc with no restart, and Alpine 3.24 GPU desktops should lose it at every session start (0.10.0 exits on a detach unless started --detached; derived from source, not seen on a device) -- both fixed only by the next build; candidate release note.
- **Settled 2026-10-01: the device-only failures carried since 555.** Both were real kernel bugs. `mount_bind_rbind` (`rbind.self_mounted`): /proc/mounts and mountinfo printed global paths inside a chroot instead of paths relative to the task's root (and listed mounts outside it) -- FIXED 6540b7fb, test chroot_mountinfo. `futex_timeout_duration` (i386 FUTEX_WAIT_BITSET deadlines): a 32-bit guest's time64 timespec has a padding word after the 32-bit tv_nsec that libc never fills; AOK read it as the high half of tv_nsec, so any nonzero stack garbage there made the call EINVAL (futex, clock_nanosleep, ppoll, pselect6, rt_sigtimedwait, epoll_pwait2, timer/timerfd settime, clock_settime, utimensat) -- FIXED 3d542653, test time64_nsec_padding. Verified on the M4 with each Alpine 3.23.3 root BOOTED (ISH_BOOT_ROOT): all four archs pass both, i386 three runs out of three. Device setup for booted Alpine legs: the Alpine roots on the M4 now carry OpenSSH on port 1022 (busybox init respawns `sshd -D`; root login by key) -- each root has its own host key at the same link-local IP, so use a known_hosts file per root and `-l root` to the IP directly (the m4ll alias checks the Devuan host key).
- **Chinese support follow-ups (feedback of 2026-10-04 done: 3557658e IME, 7fce57e7 UI in nine languages, 7f58bd40 setup-locale.sh + /proc/ish/languages, README/README_ZH/README_KO).** Still open: (1) on a device with a hardware keyboard (M4 iPad, 00008132-000948C93483001C), check that arrows, Esc and Return act inside a Pinyin composition rather than reaching the guest -- TerminalView narrows -keyCommands while composing, but the simulator tool cannot send those keys; also try the Japanese Romaji/Kana keyboards (same marked-text path, unverified) -- Korean does not use marked text on iOS (jamo, then Backspace + syllable; verified in the sim, unchanged behaviour). (2) A native speaker's pass over the machine translations, zh-Hans and zh-Hant first (app/*.xcstrings; Xcode's String Catalog editor works). (3) Strings left English because they double as keys (listed in this bullet's history, 7fce57e7): give the equalizer presets (AudioPlayerEngine) and theme names display-name maps; the LLM agent status words need a display/model split.
- **Stale TLB after exec: not reproducing (2026-10-09), and one cause closed.** 48 runs of concurrent_exec_tlb (4 and 8 copies at once, i386 root, Mac) gave no unwinding heal on the build before or after; the 2026-10-04 rate is gone. One way in was real: each address space's change count was seeded with a consecutive id and counted up by one per change, so two counts met easily (seed 100 + 50 changes = seed 105 + 45), and a new mm at a freed one's address kept its TLB entries (tlb_refresh compares only mmu and count). The id now goes in the high 32 bits (emu/memory.c). The original notes:
- **Find why exec leaves stale TLB entries so often.** concurrent_exec_tlb (8-way fork+exec of cc1, 32 rounds) takes ~1 unmapped guest-execution fault per run on the i386 CLI (36 in 40 runs, printk "guest-execution bad access ... unwinding", 2026-10-04). Each is healed by jit_unmapped_guest_fault's flush-and-retry, and since 1342669c8 that works on the CLI too (only SIGBUS used to reach the handler; these are SIGSEGV and killed the CLI ~1 run in 10). But a retry is a heal, not a cure: something hands a running task TLB entries for pages its new mm does not have. tlb_refresh skips the flush when tlb->mmu and its change count match -- the #469 shape (a new mm at the old mm's address, a colliding count). Next: log mmu pointer and changes at each unwind, and check whether the faulting task had just exec'd.
- **The amd64 JIT is 3.6x slower than the i386 one on integer code; specialise it the way i386 is.** M4, 2026-10-04, same Alpine 3.24.2 7-Zip `7z b 1 -mmt1 -md22`: arm64 995, i386 596, amd64 164. Python microbench (fib/method/dict/str, ms): arm64 91/467/364/373, i386 166/731/592/613, amd64 333/1466/1292/1242. ISH_TRACE_AMD64_JIT shows no interpreter fallbacks in either, so it is structural. Host profile of 7-Zip on amd64: eager flag computation 28% (amd64_cached_set_addsub_flags builds CF/OF/AF/ZF/SF/PF with a branch per flag after every add/sub; i386 deposits lazy flags), memory ops 33%, low-8 register cache staging 7%, set_rip 4%. Every register operand is chosen at run time by a chain of up to eight `cmp`/`b.ne` (amd64_cached_read/_write) and every size by another chain -- data-dependent branches the host mispredicts. Plan, in order of payoff: (1) flag liveness in the amd64 frontend -- a flag-setting instruction whose flags the next flag writer overwrites before any reader in the block emits a no-flags gadget variant; (2) per-register (and per-size) gadgets for the hot shapes (mov load/store, arith reg,reg/imm, cmp+jcc, lea), as i386 has, instead of the decoded-operand gadgets; (3) then the i386 fusion playbook. Measure each with the same interleaved A/B (an `amd64_jit_fuse`-style /proc switch exists for incdec_reg). **Progress 2026-10-04/05 (M4 7-Zip 164 -> 795, 4.8x, past i386's 596; Python microbench 333/1466/1292/1242 -> 93/449/404/364 ms, level with the arm64 guest's 91/467/364/373):** (1) dead-flag elimination done (`deadflags`, +26%); (2) register moves, lea [base+disp]/[rip+disp], add/sub/or/and/xor reg,reg and reg,imm specialised per register (`movr` +7%, `arithr` +11%; CMP stays generic for the cmp+jcc fusion). **Tried and measured slower, not kept:** keeping the x20-x27 cache resident across chained blocks (block-entry gadget that loads only if not live, sync instead of flush at jmp/jcc, single-register reloads after memory writes, jit_exit write-back): 7-Zip 243 -> 228, Python -8% -- the extra dispatches (an entry gadget per block, a reload after every load) cost more than the eight-register load they saved. Also flat: a cache-preserving sync around in-block memory operands alone. **Then (M4 7-Zip 245 -> 389):** mov and add/sub/cmp with [cached base + disp] or [rip + disp] on the live register cache (`memr`, 245 -> 255, with a host-fault spill of the cache: jit_amd64_fault_spill), and the flag macros made branch-free (255 -> 389, Python -8-10%) -- the per-flag branches were mispredicting on data, which the 28% "eager flag computation" line above was really measuring. **Then (389 -> 578):** each branch gadget chains through its own dispatch copy (+2.5%); shifts per register, and dead-flag elision through a shift by a known nonzero count (+10%); every register-specialised family takes all sixteen registers, r8-r15 read and written in their CPU_amd64_regs slot so they no longer flush the cache (a third of 7-Zip's instructions touch r8-r15; +7%, then +3% for the memory families, which must still load the cache first -- their slow paths spill x20-x27 unconditionally); inc/dec per register (+2%); indexed addresses through an amd64_ea gadget that leaves base + index * scale in x3 for the next gadget, plus 16-bit and byte moves, byte cmp/test and lea on all sixteen (+17%). Families are assembler macros (amd64_r16_*); math.S's object grew 2.1 -> 2.9 MB of text. **Then (578 -> 651):** cmp with r8-r15 and cmp [m],reg on the cache (flat to +1%), push/pop and inc/dec [mem] on the cache (+2.5%), blocks start with the cache loaded (jit_enter_amd64; +4%, Python 3-9%), chained branches leave it unwritten -- blocks start dirty, the chain's exits to C spill it (+5%; jrcxz must still write back, it reads RCX from memory: missing that crashed cc1 while the suite passed) -- and ret enters its target through jit_frame.ret_cache as arm64's does (+1%, Python 4-6%). **Then (651 -> 795):** indirect jmp/call through the return cache and logic load-ops (+4%), imul per register and cmp+jcc fused per register pair (+11%), test/neg/not/cmp [m],imm and every cmp per register (+2%), movzx/movsx per register including ah..bh (Python's `movzbl %ah` was going to a C helper: Python 10-30%), mov/op [mem],imm on the cache, %fs:disp/%gs:disp operands (the stack-protector canary) on the cache (Python 3-4%). **Tried and reverted:** gret_nomem (no dmb ishld) for the amd64 gadgets that read no guest memory, as i386 has it -- flat on the M4 (770/769/769 vs 763-772) and on the A10X (bip, alternating app builds: 175/176/175 vs 188/171/174), so not worth a weaker barrier. bip's Alpine3.23.3-x86_64_2 root now has 7zip and python3 for device A/Bs; a `devicectl install` that hangs there wedges CoreDeviceService for every device (killing that service clears it; a power cycle fixed bip). What is left on 7-Zip: jcc (~12%, the branch itself), the amd64_ea dispatch for indexed operands (5%), the remaining mid-block flush/reload (~5%: op-to-memory reg RMW, cmov, setcc, string ops), set_rip (1%). **Then (2026-10-05, 790 -> 808, Python 93/447/404/370 -> 84/444/376/332):** test reg,imm, mov reg,imm (the lea table's movi gadgets), movsxd reg,reg and reg,[mem], rol/ror reg,imm, setcc (low byte and ah..bh), cmp/test m8,imm8 and xor reg,reg (as and reg,0, which keeps its _nf twin) per register on the cache; test+jcc fused per register pair like cmp+jcc (amd64_ftr/fti). Finding what still flushes: log a line from gen_amd64_flush_reg_cache under ISH_TRACE_AMD64_JIT and count the emitter traced just before it (the flushes after a jcc/jmp/call/ret are block ends that never run). cmov reg,reg in two gadgets on the cache, the condition into x3 (amd64_cc) then a select per register pair (amd64_csel), instead of a flush around the generic one: 7-Zip 808 -> 821, Python method 444 -> 428. cbw/cwde/cdqe/cwd/cdq/cqo, xchg rax,reg and bswap on the cache, and a write-back without dropping the cache (gen_amd64_writeback_reg_cache) before gadgets that read guest registers from memory but write none -- the SSE loads/stores, op m8,imm8, logic op [m],reg, bt: 7-Zip flat (822/813/813), Python method 428 -> 421.
- **x87 is 3.8x slower than SSE on the i386 guest** (measured before the 2026-10-06 gadget port, which made the x87 1.3x faster on i386 and 1.7x on amd64; re-measure). M4 n-body double loop, ns per interaction: arm64 57, i386 SSE2 158, i386 x87 (-mfpmath=387) 600; amd64 SSE 105 since 1b952a7c4 (sqrtsd was interpreted: 2996). Alpine's i386 gcc emits SSE2 for new code, but Debian/Devuan i386 (no SSE2 baseline) and musl's i386 libm use x87, which runs as a C helper call per instruction (fhelper_*, emu/fpu.c, 80-bit). A gadget path for the double-precision common case (fld/fst m64, fadd/fmul/fsub/fdiv with precision control at double) is the lever; 80-bit exactness rules make it careful work. **Parked 2026-10-05:** Linux leaves the x87 control word at extended precision (0x37f) and neither glibc nor musl changes it, so a double-precision-control fast path would almost never apply; real x87 code is at most ~3% of i386 instructions (Devuan profiles), so even a full gadget x87 is worth ~1-2% there. The SSE moves above were the bigger i386 lever.
- **Settled 2026-10-09: the NaN sign of SSE add/sub/mul/div invalid results is right.** inf-inf, 0*inf, 0/0 and NaN propagation in add/sub/mul/div sd/ss/pd/ps, min/max/sqrt sd and their VEX forms give camd's bits exactly, both ABIs (a probe hashing every result; the gadget port's float helpers already produce x86's indefinite QNaN, as sqrtsd's 1b952a7c4 did).

## Diagnosed, not fixed

### A native bash ignored SIGKILL (unconfirmed, seen once)

While reproducing the above, a `/AOK/native/bash` at pid 16 survived
`kill -9 16` and was still in `ps` a second later; it went away only on a cold
boot. Not chased and not confirmed -- the kill's own exit status was not
checked -- and it may be the known shape of a native program blocked in a host
read rather than a regression of the fix in [[native-spawn-unkillable-task]].

**Retry it before believing it.** That observation predates the blocking-path
work later the same day: a task blocked reading a pipe or a socket was deaf to
the checkpoint freeze (fs/sock.c and fs/real.c asked only about guest signals,
bfbefefc8), and a long guest timeout left the host wait unbounded (251465dba).
A native bash sitting in a host read was in exactly that state, so "ignored
SIGKILL" may simply have been the same deafness seen through a different lens.
Reproduce on a current build first; if it still happens, the freezer's host
backtrace (ISH_CHECKPOINT_DEBUG, [[stuck-task-host-backtrace]]) will say where
it actually is rather than leaving it a mystery.

### EPOLLET and SOCK_SEQPACKET: what the 2026-09-25 fixes left

The edge-triggered epoll fix (fs/poll.c `poll_drain_host_locked`) and the
SEQPACKET framing (fs/sock.c `struct unix_seqpacket_hdr`) left three measured
divergences, each judged not worth its cost yet.

**Established**:
- A read that leaves data behind in a host pipe or FIFO re-arms the host's read
  event (Darwin's `pipe_read` and its fifofs both do it; measured with a bare
  kqueue, sockets do not), so an `EPOLLET` reader that does a partial read is
  told `EPOLLIN` once more with no new data. Linux says nothing. Harmless to a
  reader that reads until `EAGAIN`, which an `EPOLLET` reader must;
  `epoll_edge_triggered` checks partial reads on sockets and ptys only.
  Filtering it means telling the re-arm from a real write, which needs a byte
  count kept across every guest read of the pipe -- and getting that count wrong
  loses a real edge, which hangs, where the re-arm only costs a wake.
- dup'd fds registered in one epoll, one `EPOLLET` and one not, share a host
  watch that has to be level-triggered for the level one, so the edge-triggered
  one behaves level-triggered (`poll_fd.host_edges` stays false for both).
- A SEQPACKET `MSG_PEEK` does not deliver the message's `SCM_RIGHTS`; the real
  read does. Linux clones the descriptors for a peek. AOK's unix datagrams
  behave the same way, on purpose (see the peek comment in
  `sys_recvmsg_guest_abi`).

**Next step**: none until a program is seen to need one of them.

### The app's UI thread impersonates a guest process

`current` is per-thread, and on the app's main thread it is whatever the last
`TerminalViewController.startSession` left there -- the session's own first
process -- or init after a boot. The thread is not that process, and kernel
code it calls acts as one.

**Established** (2026-09-11): clearing it is a one-line change and it does not
work. Suspend to disk needed the UI thread not to be mistaken for a task
(`ckpt_freeze_all` skips `current`, so the backgrounding save skipped the
session leader). Clearing it in `startSession` fixed that and crashed
Settings -> Appearance the same day: `pty_slave_init_inode` reads
`current->euid`, and the appearance preview creates its pty from the UI thread.
The audit that followed found the surface is wide -- `CurrentRoot`,
`AudioLibrary` and `MotePadDocumentStore` all call `generic_open` /
`generic_statat` / `generic_renameat` with `AT_PWD` from that thread, resolving
paths through `current->fs` and checking permission as `current->fsuid`, none of
them borrowing a task first.

So the clearing was reverted and the checkpoint instead clears `current` for
the duration of its own call (`checkpoint_save_external`), which is correct and
scoped.

**Next step**: give every app-side kernel caller on the UI thread
`AppDelegate`'s `pushUsableInitTaskAsCurrent` / `popCurrentTask`, which two
callers already use (`UpgradeRootViewController`, and `AboutAppearance` since
this crash). Then clearing `current` in `startSession` becomes safe and the
invariant is "the UI thread is not a process; borrow one if you need to be".
Worth doing as its own change with the whole surface audited, not as a rider on
something else.

### Under `set -T`, some bash re-launches announce a command twice

The last trap divergence between a re-launched subshell and a forked one, and
the only one left after the 2026-09-07 work on `deps/bash/aok_fork.c` (the
special traps emitted last, emitted DEBUG-last among themselves, and `$?` moved
out of the state script into `AOK_BASH_STATUS`).

A re-launch from `execute_simple_command` -- a pipeline element, an async simple
command -- fires the DEBUG trap in the parent (that site runs the trap and *then*
calls `make_child`) and again in the child, which re-parses the text it was
handed. `set -T; trap 'echo T' DEBUG; : | cat` fires 5 times against a fork's 3.
`( )` and `$( )` are exact, because the parent does not announce those, and
`tests/manual/native_bash_fork_state.sh` asserts that agreement.

Only reachable under `-T`, which is what a DEBUG-trap debugger sets. The obvious
mechanism -- the child skipping a counted number of fires on a signal from the
parent -- can *swallow* a real fire if the count is ever wrong, which is worse
for a debugger than an extra one, so it has not been built. Anything that closes
it has to work the other way round: the child would have to be told that the
command it is about to parse has already been announced, which is a property of
that one re-launch site rather than a count.

### atop's accounting daemon wedges boot, and nothing after it starts

Reported from a device 2026-09-03. Symptom looks like broken networking --
`ssh` to the guest is refused on every route -- and the console shows
`receive NETLINK family, errno -2` on boot. Neither is the actual fault.

The thread dump names it exactly. `atopacc` sits in `ppoll` with a NULL
timeout, so it waits forever with nothing able to wake it; `S01atop` and `rc`
are both parked in `wait4` behind it. Every init script ordered after
`S01atop` therefore never runs, and sshd is one of them. The connection is
refused because sshd was never started, not because networking is broken.

The netlink line is the same event seen from the other end. AOK's generic
netlink controller (fs/sock.c, around the `GENL_FAMILY_TASKSTATS_` block)
implements exactly one family, `TASKSTATS`, because iotop is the consumer it
was written for and iotop hard-exits without it. atop's accounting daemon
asks for its own `netatop` family instead, correctly gets ENOENT, and then
blocks rather than giving up.

**The interesting half is why it blocks.** A real Linux without the netatop
module returns the same ENOENT and atop copes there, so the divergence is not
the missing family. The likely cause is that AOK's netlink socket never
becomes readable and never reports an error after a failed family lookup, so
a poll on it has nothing to return -- worth confirming before fixing, since
the fix differs depending on whether the socket should report POLLERR or the
lookup should fail the socket outright. **Unverified.**

Not a regression: atop is newly added to that root's boot sequence, so this
is the first time the path has been exercised. Removing `S01atop` restores
boot and sshd. Implementing the `netatop` family is NOT the fix -- a guest
that blocks forever on a family a real kernel also refuses is the bug.


### FUSE has no attribute cache, and three absences follow from it

Measured against Devuan (Linux 6.12) on 2026-09-01, when mmap, FUSE_FORGET and
FUSE_INTERRUPT were added (`fs/fuse.c`, `tests/manual/fuse_basic.c`).

AOK's VFS is path-based, so every FUSE operation walks from the root nodeid
with one `LOOKUP` per component and forgets each node behind it. There is no
dentry cache and no attribute cache. Three things follow, and all three are
currently absences rather than half-implementations:

- **`readdirplus` would cost more than it saves.** It returns each entry's
  attributes with its name, which is what makes `ls -l` one request instead of
  N. With nowhere to keep them the attributes would be fetched and dropped,
  while every entry it returned would still be a reference to forget. Nothing
  to do here until there is a cache.
- **A path five components deep is six requests.** Same cause. The design note
  in `fs/fuse.c` argues the trade was right to make first, and it was, but the
  measured cost is real on a deep tree.
- **A name that does not exist costs nine requests, not one.** Measured
  2026-09-17 with the daemon in `tests/manual/blocked_wait_state.c`: one
  `stat` of a missing name in the mount root sent READLINK and GETATTR of the
  root, the LOOKUP, then READLINK, GETATTR, READLINK and GETATTR of the root
  again and the LOOKUP twice more. Linux 6.12 (camd, as root) sent the one
  LOOKUP. Two things are separable from the cache: READLINK of a node whose
  attributes already say it is a directory, and the walk being repeated after
  a failed lookup. The repeat is also what kept a poke's spurious EINTR on a
  FUSE request from reaching the guest before `wait_for_blocked` (the daemon
  still got the FUSE_INTERRUPT), so find out what it is for before removing
  it.
- **A mapping and `read()` are coherent only at sync points.** `mmap` is backed
  by a per-nodeid host stand-in for the page cache (see the chapter), while
  `read`/`write` go straight to the daemon. Linux's page cache makes the two
  coherent continuously. Routing reads through the stand-in would change
  behaviour for daemons that return different bytes on each read, so it needs
  the same cache design rather than a local patch.

The one change behind all three is a real dentry/attribute cache with FUSE's
`entry_valid`/`attr_valid` timeouts honoured -- which also means owning node
lifetimes across a boundary where the other side may crash. The reference
accounting that makes that safe now exists and is asserted by the test (no node
is ever forgotten more times than it was handed out), so the groundwork is
there; the cache is not.

One other absence is unrelated to this and is simply not worth it: **splice**
on `/dev/fuse` (the transfers are not copy-bound). (The **`fsopen()`-based
mount API** works since 2026-10-09: tests/manual/fuse_fsopen.c.)

### Darwin compresses our memory already, and it buys us nothing

Measured 2026-09-09 on this Mac, because "should AOK compress guest memory?"
turns on whether the host is already doing it for us. It is -- and the result is
the opposite of the obvious one.

**Darwin compresses idle anonymous memory with no memory pressure at all.** A
process that dirties 1 GiB and then does nothing has essentially all of it in
the compressor within ~25 seconds:

```
COMPRESSIBLE (repeating byte)      INCOMPRESSIBLE (random)
after dirtying  fp=1025.4 c=   0.0    after dirtying  fp=1025.4 c=   0.0
after 25s idle  fp=1025.4 c=1024.2    after 25s idle  fp=1025.4 c=1023.9
```

(`fp` = `task_vm_info.phys_footprint`, `c` = `.compressed`, MB. Interleaved A/B,
two rounds.)

**And `phys_footprint` does not move.** 1025.4 MB in every arm -- whether the
gigabyte compresses ~infinitely or not at all, whether the compressor has taken
it or not. The ledger charges for the pages regardless of how well they
compressed.

**Why that matters more than it looks.** jetsam kills on `phys_footprint`
(platform/darwin.c says so, and the swap budget is measured against it, not
RSS). So the host's compression -- which is already happening, for free, to
every cold guest page -- **buys AOK no headroom whatsoever**. It cannot be
relied on to keep the app alive, and no amount of making guest memory more
compressible will help by itself.

**Which inverts the design question.** AOK-level compression is not redundant
with the host's; it is the only kind that can help, because AOK would compress
into its *own* smaller buffer and then actually release the originals -- and
released pages do move the footprint. That is zram's model: hold N pages'
worth of data in a pool of roughly N/ratio, and the footprint falls by the
difference. It also composes with the pager: compress in RAM first, and only
spend flash when the compressed pool is full, which cuts writes by the same
ratio (see the swap write budget).

**Not yet established, and needed before building anything:** the compression
ratio and CPU cost on *real guest pages* rather than synthetic ones, the added
latency on the fault path, and confirmation on a device that iOS's
`phys_footprint` behaves as macOS's does here. The measurement above is macOS
and uses `malloc`, not guest memory through AOK's page tables.

### Guest-memory compression: BUILT (phases 1-2), and what is left

**Phases 1 and 2 are in** as of 2026-09-09. `kernel/zpool.c` is a size-classed
pool for compressed frames (host unit test, `meson test -C build zpool`), and
`kernel/zswap.c` puts it in front of the swap area by intercepting
`swap_slot_write`/`read`/`free`. Nothing about eviction eligibility, the fault
path, fork/COW or the address-space barrier changed -- that seam was already a
backing-store interface, which is why the integration is three call sites.

Verified end to end by `tests/manual/zswap_roundtrip.c`: 24 MB survives a
compressed round trip byte-for-byte, and the test FAILS rather than skips if no
frame went through the tier. Run at a 1 MB cap it also covers the mixed case --
640 frames held in RAM, 896 overflowed to flash, all correct.

**UNDER A REAL DATABASE, WITH DECOMPRESSION ON THE CRITICAL PATH.** The runs
above evicted memory and left it alone; nothing faulted back in any volume, so
the decompress path was barely exercised (46 loads). This one puts it under
sustained load. MariaDB 11.8.6 on the same iPad, 512 MB InnoDB buffer pool,
sysbench 1.0.20 `oltp_read_write` over 2 tables x 394k rows (188 MB), two
threads.

Memory was pushed down until kswapd evicted part of the buffer pool -- reclaim
fired at headroom 478 MB against the 482 MB watermark, **the second independent
confirmation of that threshold** (nothing at 494, fired at 478) -- then sysbench
was run against a buffer pool that was partly compressed:

```
                      TPS    avg ms   95th ms   errors   zswap loads
cold baseline        3.48    571.89    787.74        0     64 ->  64
partly compressed    4.42    452.25    707.07        0    162 -> 674
```

**512 frames were decompressed on the fault path during a 90-second run, with
zero errors.** That is the correctness result: sysbench's point-selects and
updates run against InnoDB pages that went out through the compressor and came
back, and a decompression fault would surface as a query error or a wrong row,
not silently. It did not.

`declined` stayed 0 and `bytes_written` stayed 0 across the whole exercise, so
real InnoDB pages compress as well as the synthetic ones did and none reached
flash.

**Do NOT read the TPS column as "compression makes it faster."** The two runs
are not a controlled A/B: the baseline ran immediately after the data load with
a cold buffer pool doing real disk I/O, and the second had a warmer one. The
defensible claim is the weaker one -- **serving 512 faults from the compressed
pool cost no measurable throughput or latency** -- and that is the question a
user actually has.

**Ratio on a mix including real InnoDB pages: 2.42x.** The reclaim added 7,278
frames (113 MB of guest memory) for 46.8 MB of pool growth. That is close to the
2.38x measured for cc1 on the Mac and above the 2.11x of the synthetic run, so
database pages compress at least as well as the text records did.

**THE CLEAN DEVICE RUN, 2026-09-09.** iPad 5th gen (A9, 1.45 GB), app in the
foreground, no debugger attached, swap 1 GB, pool cap 128 MB, enabled from
Settings. A single non-forking probe (`zprobe`) allocated 64 MB at a time and
sampled after each step:

```
+768MB   headroom=490MB  stores=0     poolKB=0      bytes_written=0
+832MB   headroom=453MB  stores=3754  poolKB=28960  bytes_written=0   <- engaged
settle+10s  headroom=484MB stores=7874 poolKB=61744 bytes_written=0
settle+180s headroom=483MB stores=8132 poolKB=63776 bytes_written=0
```

**Four things, and all four are what the feature promised:**

1. **Reclaim engaged exactly where predicted.** The watermark is
   `available < 2 x host_mem_headroom_floor` = 482 MB. Nothing happened at
   490 MB; it fired at 453 MB. The threshold is not approximately right, it is
   right.
2. **Zero flash writes.** `bytes_written` stayed at 0 for the entire run, and
   `declined` stayed at 0 -- every single evicted frame went to RAM. On a
   feature whose headline objection is flash wear, that is the number.
3. **THE FOOTPRINT ACTUALLY MOVED.** Headroom recovered 453 -> 484 MB and held
   there for three minutes. This is the measurement the debugger would have
   destroyed: `MADV_FREE_REUSABLE` returns success while moving no ledger under
   an attached debugger, so a recovering headroom is the only proof the frames
   were released rather than merely accounted for.
4. **It reached equilibrium and stopped.** 484 MB is just above the 482 MB
   watermark, and reclaim ceased there rather than continuing to evict. The
   pool used 60 MB of its 128 MB cap. That is a pager doing the right amount of
   work, not the most.

**The effective ratio is 2.11x, with fragmentation counted.** 8132 frames x
16 KiB = 127 MB of guest memory held in 61,744 KB of pool. That is the honest
figure -- `poolKB` is what the slabs occupy, so size-class waste is already in
it. It sits just below the raw 2.38x measured for cc1, which is what ~10%
fragmentation predicts. The probe's data is structured text records, so it is
somewhat more compressible than a binary heap; treat 2.11x as a good case and
not a ceiling.

**One number is not fully explained and should not be smoothed over.** 127 MB of
frames released into 60 MB of pool should free about 67 MB, and headroom
recovered 31 MB (memfree agrees: +32 MB). The gap may be `MADV_FREE_REUSABLE`
pages counting as reusable-but-not-yet-free, or accounting differing between the
two figures. Not chased, and flagged rather than averaged away.

**IT WORKS ON A DEVICE, UNDER REAL PRESSURE.** Measured on the iPad 5th gen
(A9, 1.45 GB) on 2026-09-09, with the tier enabled from Settings at a 128 MB cap
and swap at 256 MB. Memory was consumed until the machine crossed kswapd's
watermark -- which is `available < 2 x host_mem_headroom_floor`, so 482 MB
against the 241 MB floor:

```
headroom 483 MB   pressure WARN (throttle engaged, growth still allowed)
t+10s   stores=13419   bytes_written=0   headroom=482
```

kswapd engaged at the predicted threshold to the megabyte, evicted **13,419
frames** -- 13419 x 16 KiB, about 210 MB of guest memory -- and
**`bytes_written` stayed at 0**. Not one byte reached flash. That is the whole
claim of the feature, on real hardware, under pressure that arrived on its own
rather than being forced through a development control.

**AND THEN THE DEVICE STOPPED ANSWERING -- BUT NOT, IT TURNS OUT, BECAUSE OF A
JETSAM KILL.** An earlier version of this entry said it was one. That was not
established and is probably wrong: the maintainer found the app **backgrounded,
not dead**, and iOS destroys a backgrounded app's listening socket, which is
already documented in this file. "Connection reset", then "timed out during
banner exchange", is exactly what that looks like from the other end -- and it
is a far better fit than the memory-pressure story I reached for, which was that
fork was failing under pressure. Both readings explain the symptom; only one of
them was checked, and it was not mine.

**So no jetsam kill is confirmed at any point in this work.** What is confirmed
is that the app stopped being reachable over ssh while under heavy memory
pressure, twice. The practical consequence for anyone repeating this: keep the
app in the FOREGROUND for the duration, or the guest is suspended out from under
the measurement.

There is still a design point worth keeping, and it does not depend on how the
app stopped:

**THE POOL IS RESIDENT MEMORY AND COMPETES WITH WHAT IT SAVES.** Compressing
210 MB into a pool of at most 128 MB saves at most 82 MB; it does not save 210.
The pool's own bytes are charged to `phys_footprint` exactly like the frames it
replaced. So a cap that is generous relative to the device's RAM can make
pressure worse rather than better, and the ceiling this shipped with -- 4 GB,
chosen against swap's 16 GB -- is far too generous for a 1.5 GB device to be
offered without guidance.

**AND THE BINDING CONSTRAINT WAS THE SWAP AREA, NOT THE POOL.** ktop's last
frame before the kill settles it:

```
Mem[  1.18G/1.42G ]      Swp[  208.2M/256.0M ]
  827  /tmp/cold 400   VIRT 411728  RES 305232
  882  /tmp/cold 450   VIRT 462928  RES 356432
```

208 of 256 MB of slots were consumed -- 81% -- with `bytes_written` still 0, so
every one of those slots held its data in RAM. About 190 MB had genuinely left
the two cold processes (VIRT minus RES). But **slots are allocated per evicted
frame whether or not the bytes go to flash**, so the 256 MB area was about to
run out, and when it does eviction stops completely however much pool is left.

That is the sizing rule, and it is not the obvious one:

- **The swap area size caps how much memory can be evicted at all.** It is the
  address space of the pager.
- **The pool size decides how much of that costs RAM instead of flash.**

So the two are not alternatives and the area cannot be made small (which an
earlier version of this entry wrongly suggested). Against 850 MB of demand, a
256 MB area could never have kept up no matter what the pool did.

**Open, and it is the next thing to settle**: whether the pool cap should be
clamped against device RAM, and whether the UI should relate the two sizes at
all rather than offering them as independent numbers. Unresolved here because
the evidence does not distinguish "the pool made it worse" from "the area was
too small and I allocated too much too fast" -- and picking one without the data
is exactly the kind of story this file exists to prevent.

**ZRAM VERIFIED ON A DEVICE, 2026-09-09.** Everything about the file-less mode
had been proven on the CLI, where /proc/ish/swap_evict can force an eviction --
a control that is EPERM on an installed app by design. So on the iPad 5th gen,
with swap OFF and compressed memory ON from Settings, memory was consumed until
kswapd engaged on its own and then every allocated region was read back:

```
loads      201 -> 11389     11,188 frames faulted back OUT OF THE POOL
bad bytes  0                every one byte-for-byte correct
stores   14038 -> 25373
bytes_written  0            nothing reached storage, because there is no file
```

That is roughly 175 MB of guest memory compressed, released, and restored
exactly, on the oldest hardware AOK supports, with no swap file in existence.

**THE FIRST ATTEMPT REPORTED PASS AND PROVED NOTHING**, which is worth recording
because it is the third instance of the same mistake in this feature's history.
It verified only the region it had filled first, and reported `loads 70 -> 70` --
the counter never moved, so the bytes it checked had never left RAM. 14,038
frames had been evicted; none of them were the ones being checked. The pass
condition was `bad == 0 && stores > 0 && bytes_written == 0`, which neglected the
one thing that mattered.

Fixed by verifying EVERY allocated region rather than one, and by requiring
`loads` to move for a PASS -- it reports INCONCLUSIVE otherwise.

**The rule, stated because it caught three separate green results here:** for a
feature that only acts under a condition, the test must assert THE CONDITION WAS
REACHED, not merely that nothing broke. `swap_roundtrip` passed with 4096 frames
declined and 0 stored; a sysbench run showed 3.75 TPS with the tier never
engaging; and this reported clean bytes that never left memory. In all three the
counters, not the assertion, were what exposed it.

**Also observed: reclaim lags a fast allocator.** `stores` stayed at 0 through an
entire 896 MB allocation and only climbed once it stopped. That is the
second-chance clock working as designed -- a frame must survive two sweeps
untouched before the third may take it -- but it means the tier protects against
sustained pressure rather than a burst that outruns kswapd. Worth saying in
release notes, so a user who hits a limit during a fast allocation does not
conclude the feature is broken.

**RAM-ONLY IS BUILT** (2026-09-09), so there are two modes and both ship:

| Settings | mode | storage cost |
|---|---|---|
| swap on + compressed memory on | **zswap** -- pool in front of the file | area preallocated, writes ~0 |
| swap **off** + compressed memory on | **zram** -- pool is the only storage | **none, ever** |

No new switch: "Enable Compressed Memory" works either way. The zram mode exists
because requiring swap was an awkward ask -- enabling swap costs flash
immediately, since the area is `F_PREALLOCATE`d and `ftruncate`d to full size
before a page is written, so a user whose worry is wear or free space had to
hand over a gigabyte of storage to turn on the feature whose point is not
writing to storage.

It was small because the eviction path was already right: a refused
`swap_slot_write` frees the slot and leaves the frame resident
(emu/memory.c:3669), so "pool full" and "does not compress" simply mean that
frame stops being evictable. Nothing lost, nothing written. What made it
invasive was `swap_fd >= 0` doing double duty as "does an area exist" -- six
sites meant that and now ask `swap_area_live_locked()`, which tests the bitmap,
allocated and freed with the area in both modes.

**In zram mode an incompressible frame is simply never evicted.** There is
nowhere for it to go, and that is correct rather than a limitation: it stays
resident, exactly as it would with the feature off. Demonstrated by
`swap_roundtrip` SKIPPING in that mode -- its pattern is a per-byte hash, so all
4096 frames were declined and none moved.

**All four configurations verified separately**, because they exercise different
paths: default (201/201 guest suite, zpool unit test); swap only (round trip
PASS with 67 MB genuinely written, so the file path still does real I/O); zswap
(all three tests PASS); zram (round trip and fork invariant PASS, zero bytes
written).

**The Settings design follows from that**: the two sizes are separate knobs, and
the swap-file one should be allowed to be small rather than implying a large
area. Not yet wired -- `ISH_GUEST_ZSWAP_MB` is a launch variable, so the tier is
reachable from the CLI and Xcode and not from an installed app.

### 555: zram was lazy, because it inherited a swap file's caution

**"It reached equilibrium and stopped" was recorded above as the fourth thing
the feature got right. For a swap FILE it is. For RAM-only it was the bug.**

Measured on the ip5 device on 2026-09-10, with compressed memory on, swap off,
and the pool cap raised to its maximum from Settings:

```
kswapd   running, 864 passes, 0 bytes reclaimed
pool     0 KB of a 494 MB cap        headroom 931 MB    ceiling 1450 MB
write_window  0 of 4294967296 bytes used in the last 24h
```

**864 background passes, a half-gigabyte pool the user had deliberately asked
for, and not one frame ever compressed.** The watermark was
`available < floor * 2` = 482 MB, and the app sat at 931 MB.

Every gate around eviction was designed for a file: reclaim writes to the user's
flash, that is metered against a 24-hour budget, and flash wears out. Waiting
until the app is nearly dead is right when each eviction costs a write. **In
RAM-only mode none of that is true.** `swap_write_frame` takes the `ram_only`
branch and returns `_ENOSPC` before it reaches a file descriptor -- which is
what `write_window 0 of 4 GiB` after 864 passes actually says.

**A watermark was the wrong SHAPE, not merely the wrong number.** Two attempts
at one failed the same way, and both are worth recording because the second
looked convincing:

1. *Half the budget.* On the device that moves the trigger from 482 MB to
   725 MB of headroom -- and the device was at 931 MB, so it still did nothing.
2. *Half the budget with hysteresis*, a low/high pair so a run continues once
   started. That fixed a real defect on the way past -- a bare threshold does
   not shed memory, it hovers: measured with a 400 MB hog against an 800 MB
   budget, headroom sat at **401 MB against a 400 MB mark** for a minute with
   kswapd taking 24 passes and reclaiming 0 bytes. But it still begins with
   "wait until enough is gone".

Any threshold against remaining headroom encodes waiting, and there is nothing
to wait for. **So RAM-only reclaim has no watermark at all: it runs while the
pool has room and stops when it is full.** The cap is a size the user chose;
filling it with cold frames is what choosing it asked for.

What keeps that honest is not a headroom test but three things that already
existed, plus one that did not:

- the **aging clock**, which offers only frames that have read cold across
  several sweeps, so hot memory is never a candidate;
- the **thrash guard**, which pauses reclaim outright when evicted pages come
  straight back, whatever the headroom says;
- the **pool-full check**, because at capacity every further eviction
  compresses, is declined for want of room, and leaves the frame resident --
  CPU spent, nothing moved;
- a **housekeeping cadence**, which is the new one and was not optional.

**Why the cadence was needed.** A sweep is not free even when it reclaims
nothing: it takes a task snapshot and an address-space barrier per mm, and that
barrier is paid by the guest's own threads -- the same mechanism that makes
mallocng's mmap/munmap churn expensive. An empty-sweep backoff was tried first
and does not help, because the common case is not "nothing cold" but "a trickle
of newly-cold memory": measured, **60 passes in 30 s for 12 frames**, with every
productive pass resetting the backoff. So unpressured reclaim now sweeps on one
pass in four, and under real pressure on every pass. Measured after:

```
over 30 s unpressured: passes=60 sweeps=6
```

**The cost of being wrong is small, and was measured** on that device against
MariaDB's live 684 MB (175,209 pages):

```
lz4    compress   2.46 us/page   decompress   9.70 us/page
```

A 16 KiB frame costs ~10 us to compress, so the whole 494 MB pool is well under
a second of CPU, once -- roughly 0.4 J against a ~7 Wh battery. A frame faulted
back costs ~39 us: 0.4% of one core at 100 frames/s, 3.9% at 1,000. Compressing
is close to free; picking hot frames is what would cost, and the clock and the
thrash guard are what prevent that.

`/proc/ish/swap` now reports **sweeps as well as passes**, because with a
cadence those are different numbers and the gap between them is the policy.

**`zswap_fork_cow` had to change, and the reason generalises.** It asserted that
a forced sweep with a fork outstanding did not move the `stores` counter. That
became unfalsifiable the moment reclaim stopped waiting for pressure: kswapd now
runs continuously, so `stores` moves for reasons unrelated to the region under
test, and it failed on a kernel whose COW handling was correct (stores
545 -> 549, invariant intact). **A global counter cannot attribute a store to a
particular frame.** It now does what its first version did and what the counter
was only ever a proxy for: the child writes through the sharing and the parent's
view must be untouched. Immune to background reclaim, and it fails for exactly
one reason.

`tests/manual/zram_idle_reclaim.c` locks the new behaviour in, and asserts it
**where there is no pressure** -- it refuses to run unless headroom is well
above the old file-backed mark, and fails if nothing is stored while it stays
there. Testing under pressure would have passed before the fix and after it.
Measured: `stores 288 -> 545 after 11 s, with headroom never below 982 MB
against a 400 MB file-backed mark`.

**Still open:** the pool cap default is a flat 128 MB, and the maximum is
physical RAM / 4 -- which is why a device asking for 512 MB gets 494. The
maximum scales; the default does not, and the clamp is silent.

### Phase 0, the measurements the above rests on

Follows the entry above -- the host's own compression buys AOK nothing, so only
compression AOK does itself can help. `kernel/memcomp.c` and
`/proc/ish/mem_compress` measure what it would buy, on real guest pages.
Measured 2026-09-09 on an M4 Mac, every round trip decompressed and compared
(`verify_failures 0` throughout, and that guard earned itself twice -- see
below).

**cc1 compiling 28 MB of C, 90 MB resident anonymous -- the representative one:**

| algo  | ratio | compress | decompress | >=8x | >=4x | >=2x | >=1.33x | worse |
|-------|------:|---------:|-----------:|-----:|-----:|-----:|--------:|------:|
| lz4   | 2.23x |  7.06 us |    1.89 us | 3573 | 1666 | 5191 |   12732 |    22 |
| lzfse | 3.23x | 55.56 us |    9.47 us | 4580 | 3078 |14885 |     641 |     0 |
| zlib  | 3.60x | 70.90 us |   13.55 us | 5105 | 3355 |14712 |      12 |     0 |

Two other workloads for shape, neither as representative: a synthetic
malloc mix (64 MB) gave lz4 2.45x, and `sort` over repetitive text (41 MB) gave
lz4 6.50x -- that last one is inflated by the input being `yes`-generated lines
and should not be quoted as a result.

**The decision.** `lz4` for an in-RAM pool. 2.23x on a real workload for
**1.89 us to decompress on the fault path**, against the hundreds of
microseconds a read from flash costs -- so holding a page compressed in RAM is
roughly two orders of magnitude cheaper than having paged it out, which is the
entire zram argument and it survives being quantified here.

lzfse and zlib buy 45-60% more ratio for 5-7x the decompress cost. That is the
wrong trade for a hot pool and possibly the right one for what is actually
written to flash, where the cost is paid once and the ratio directly reduces
the 24-hour write budget. A two-tier design (lz4 in RAM, something denser on the
way out) is the shape the numbers point at.

**A real server, measured on the A9: mariadbd with a 131k-row InnoDB table,
293 MB resident anonymous** (out of 1.35 GB of *mapped* RSS -- the gap is
AOK's RSS counting address space, and it is worth knowing that four fifths of
what a database appears to hold is not resident at all). lz4 5.42x at 3.05 us
compress and 8.45 us decompress; zlib 9.82x.

**Read that ratio with care.** 55,760 of 75,106 pages land in the >=8x bucket,
because InnoDB allocates a large buffer pool that is mostly untouched. That is
not a measurement error -- a real server genuinely does hold that memory, and
compressing it really is nearly free -- but it means 5.42x is an *expected
benefit* number and not a *worst case* one. For CPU planning use the dense-data
figures (python on the same device: 2.83x, 3.07 us), because a page that
compresses to nothing costs almost nothing to compress. Note also that lz4's
decompress there (8.45 us) exceeds its compress (3.05 us), which is backwards
for lz4 and is the same near-empty-page effect: decompressing a trivial input
still has to write 4 KB, and on an A9 that write is what is being timed.

**Incompressible pages are a rounding error, not a design burden**: 0 pages
failed to fit, and 22 of 23,184 got no smaller. A raw-storage fallback is still
required for correctness, but it will not be a common path.

**On-device, and the earlier inversion is explained -- it was not hardware.**
Measured on an iPad 5th gen (iPad6,12, A9 at 1.07 GHz, the oldest part AOK
supports) against a real Python heap, pid verified:

| host | workload | resident | lz4 ratio | lz4 compress | lz4 decompress |
|------|----------|---------:|----------:|-------------:|---------------:|
| M4 Mac | cc1     |   138 MB |     2.38x |      6.30 us |        1.94 us |
| A9 iPad| python  |    33 MB |     2.83x |      7.61 us |        3.07 us |

**The A9 decompresses only 1.6x slower than an M4**, not the 2-3x guessed, and
on real data the algorithm ordering is the same on both: lz4 fastest, zlib
slowest. That closes the CPU gap, and it closes it favourably -- 3 us on the
slowest supported device is still two orders of magnitude under a flash read.

The inversion that prompted all this (lz4 11.41 us against zlib 5.64 on the
same device) came from **degenerate input, not from the SoC**. Those earlier
device samples were near-empty pages -- 4431 of 4500 in the >=8x bucket, ratios
of 25x and 136x -- and when a page compresses to almost nothing the measurement
is fixed API overhead rather than throughput, which does not rank the codecs the
way real data does. The first device numbers should not have been quoted, and
the lesson is the ordinary one: **a ratio of 136x is not a good result, it is a
warning that the input is not representative.**

The order-rotation added to the instrument is kept anyway. It is cheap, it makes
position and algorithm independent, and it is the thing that would have
distinguished these two explanations without needing a second workload.

**Hardware acceleration remains a real consideration even though it was not the
cause here** -- Apple's codecs are tuned per architecture and their relative
speeds need not be constant -- so the runtime pick below is still the right
design. It is now a cheap insurance policy rather than a necessity.

**What is NOT established, and none of it should be skipped:**
- **The pool allocator.** Compressed pages are variable-sized, so they need one;
  Linux uses zsmalloc and it is not small. Fragmentation overhead is unmeasured
  and eats directly into the ratio above.
- ~~Device confirmation that iOS's `phys_footprint` ignores compression.~~
  **CONFIRMED on an iPad 5th gen, 2026-09-09**, and it is the finding the whole
  case rests on. `/proc/ish/mem_guard` headroom, while a guest process held
  250 MB of a single repeating byte -- maximally compressible, the easiest
  possible case for the compressor:

  ```
  baseline  890 MB
  t+25s     722 MB   <- the 250 MB is charged
  t+70s     723 MB
  t+130s    723 MB
  t+190s    726 MB   <- never comes back
  ```

  macOS compressed the equivalent buffer within 25 seconds. iOS charges for it
  regardless, for at least three minutes of idle. **The host's compression buys
  AOK no jetsam headroom on a device**, which is exactly what it does on the
  Mac, and it means only compression AOK performs itself -- into its own
  smaller buffer, releasing the originals -- can move the number that kills the
  app.
- Only two genuinely representative workloads. A JVM, a Python, and a Node
  process would each have a different shape.

**Two instrument bugs worth remembering**, both caught by the measurement's own
guards rather than by review:
- The first run reported exactly one verify failure per algorithm, all three
  identical -- not how three independent compressors fail. The pages belong to a
  RUNNING process and were written between the compress and the compare. Each
  page is copied to a private buffer now, which also makes the three algorithms
  measure the same bytes.
- The second run **killed the emulator**: SIGBUS/KERN_MEMORY_ERROR reading a
  file-backed page whose host bytes were not there. `mem_walk_resident_pages`
  now yields only anonymous, host-readable pages. See the commit; that filter is
  a safety requirement, not a preference.

### Signals left over from moving kill() to the process queue

Fixed 2026-09-23 ("signal: a thread takes its own signals first, and kill()
queues on the process", tests/manual/signal_dequeue_order.c): a process's
signal waits on the process's queue, and a thread takes its own queue before
the process's, synchronous signals first. Found alongside, by reading, not
measured:
- A native program's handlers run in the order their signals are taken:
  `nlibc_deliver_signals_count` calls each as it takes it. A translated
  guest, like Linux, stacks a frame per signal, so the LAST one taken runs
  first.
- A stop or continue signal the kernel sends to one thread while holding
  pids_lock (ptrace's attach SIGSTOP, a resume's signal) cancels the other
  kind on that thread's queue and the process's, not on the other threads'
  own queues: `send_signal` has no thread list. tkill, tgkill and
  rt_tgsigqueueinfo do reach every thread (`signal_prepare_stop_cont_threads`).
- kill(-1) skips only the calling thread (`kill_everything`); Linux skips
  the caller's whole thread group, so a non-leader thread's kill(-1) also
  signals its own process here.

### Stop and continue notices: what is still open

Fixed 2026-09-23 (tests/manual/notify_parent_cldstop.c): a stop, a continue
and a ptrace stop are announced with SIGCHLD, not the exit signal, once, to
the leader's parent or the tracer, as Linux's do_notify_parent_cldstop does;
and a tracer's resume lifts a group-stop before the tracee wakes.

Also fixed 2026-09-23 (tests/manual/reparent_zombie_disposition.c, measured
on Linux 6.12 64-bit and -m32 first): zombies handed to a new parent at an
exit were announced with one SIGCHLD whatever its disposition, and left for
a wait a parent that disclaimed SIGCHLD never makes. do_exit's reparent loop
now goes through exit_notify_process_locked, as Linux's reparent_leader goes
through do_notify_parent: SIG_IGN or SA_NOCLDWAIT releases the zombie at
once, SIG_IGN sends nothing even to a new parent that blocks SIGCHLD, and
each zombie gets its own SIGCHLD, with its CPU times.

Also fixed 2026-09-23 (tests/manual/wait_child_order.c, measured on Linux
6.12 64-bit and -m32 first): AOK's children lists were newest-first, Linux's
oldest-first. fork, CLONE_PARENT, exec's de-thread and the reparent loop all
linked at the head, so wait(-1) reaped the youngest zombie first (Linux 1 2
3, AOK 3 2 1), a subreaper reaped its orphans before its own children, and of
three zombies reparented together the SIGCHLD a blocking new parent found
named the youngest. Now a child goes at the end, as copy_process's
list_add_tail puts it; an exit hands its children on after the new parent's
own, in their order, as list_splice_tail_init does; and a thread's exec
takes the old leader's place, as de_thread's list_replace_init does.
reparent_zombie_disposition.c now requires the oldest. A checkpoint had its
own copy of the bug: the restore builds each parent's children in image
order, and the save wrote siblings in whatever order its placement left
them (four zombies came back reaped 7 4 6 5). The save now walks the tree
for its order (checkpoint_threads.sh, mode `order`). AOK has no
/proc/<pid>/task/<tid>/children; the test checks it where it exists.

Found alongside, by reading, not measured:
- A group-stop is announced as soon as one thread takes the stop signal:
  receive_signal stops the whole process at once, and the others stop at
  their next pass through handle_interrupt -- one blocked in a syscall only
  when that call returns. Linux stops every thread first and announces the
  stop from the last (task_participate_group_stop), so a parent told
  CLD_STOPPED can find threads here still in R or S where Linux shows T.
- The notices carry si_utime and si_stime 0; Linux fills in the child's CPU
  times.

### The orphaned-group test has two copies, and neither skips what Linux does

Found 2026-09-23 by reading, while adding the orphaned-group rule's
per-child case; not measured. Linux has one test, `will_become_orphaned_pgrp`,
for exit and for the terminal (`is_current_pgrp_orphaned`, which makes a
background read or write EIO rather than a stop). AOK has two --
`pgrp_is_orphaned_locked` in kernel/exit.c and `pgroup_is_orphaned` in
kernel/group.c, which fs/tty.c's `tty_check_change_locked` calls -- and
they differ from Linux and from each other:
- The terminal's copy counts a zombie member, and one init has adopted, as a
  way back into the session. Linux skips both (`exit_state &&
  thread_group_empty`, `is_global_init`); only the exit copy skips init.
- The exit copy skips a member whose leader thread is `exiting` even while
  its other threads run. Linux counts that process until its whole thread
  group is gone, so a group whose only way back is a process whose main
  thread called `pthread_exit` is orphaned here and not there.

**Next step:** one helper in kernel/group.c, walking the group's pgroup list
(complete now that setpgid files a joiner there), with Linux's two skips,
for both callers. Tests against camd: a background read from a group whose
only way back is a zombie member (EIO on Linux), and the `pthread_exit`
way back above.

### PI futexes: the requeue half, and no inheritance

FUTEX_LOCK_PI, LOCK_PI2, TRYLOCK_PI and UNLOCK_PI are implemented (kernel/
futex.c, 2026-10-02; tests/manual/futex_pi.c, which Linux 6.12 passes on camd
in 64- and 32-bit builds): the word protocol, direct hand-off to the first
waiter, a dead owner's lock going to a waiter that was queued (marked
FUTEX_OWNER_DIED and FUTEX_WAITERS, as Linux leaves it), ESRCH for an owner
that is gone with nobody queued, the PI bit in robust-list links, and a signal
restarting the lock (ERESTARTNOINTR: glibc takes an EINTR as having it).
Before, glibc's probe -- `futex(&word, FUTEX_UNLOCK_PI_PRIVATE)`, EPERM on
Linux -- got ENOSYS, so PTHREAD_PRIO_INHERIT mutexes were refused at init
(PulseAudio's, in wf-panel, fell back to plain ones) and dmesg showed one
`FIXME Unsupported futex FUTEX_UNLOCK_PI(..., 135, 0, ...)` per program.

Left:

- **FUTEX_WAIT_REQUEUE_PI is still ENOSYS, and FUTEX_CMP_REQUEUE_PI is wrong**
  (seen 2026-09-25): it compares `*uaddr1` against `val`, the wake count,
  where Linux compares `val3`, and it wakes no one where Linux takes the PI
  lock for one waiter. Nothing current calls either -- glibc's condvars
  stopped in 2.25, musl never did -- so rewrite them on futex_lock_pi's
  waiter queue only when a caller turns up. Its waiters do keep their
  futex's reference when moved (`futex_requeue_waiters`).
- **No inheritance.** iSH has no scheduler priority to donate -- realtime
  classes are refused with EPERM (kernel/resource.c) -- so the boost is a
  no-op by design, not an omission.
- A PI waiter re-checks its owner every 50 ms (and at once when the owner
  exits, futex_exit_pi); Linux has the owner's task pin a pi_state instead.

### tmpfs size= is accepted and not enforced

Measured 2026-09-01. `mount -t tmpfs -o size=1M` takes the option and then
lets the filesystem grow without limit: 4 MiB written to a size=1M mount with
no ENOSPC, where Linux accepts 1044480 bytes and then fails. It matters more
here than on a desktop -- an unbounded /tmp or /dev/shm is host memory on a
device with a jetsam budget.

The obstacle is accounting, not the check. `tmpfs_file_resize(inode, size)`
takes no mount, and `struct tmp_inode` has no way back to one, so there is
nowhere to add up a mount's bytes. `tmpfs_statfs` already walks the whole tree
with `tmpfs_count_tree` to answer df, which is fine once per statfs and
hopeless per write. The fix is a per-mount used-bytes counter that
`tmpfs_file_resize` and the write path adjust, which means giving the inode a
pointer to its mount's accounting (set at creation, since every inode is
created under a known parent) and threading it through the four resize call
sites.

### A directory walk still costs more per entry the larger the directory

Measured 2026-09-01: 2000 entries at 5.28 us each, 8000 at 8.61 us -- a 1.63x
per-entry increase for 4x the entries, where Linux is flat (0.43 vs 0.41).

`fs/dir.c` used to call the host `telldir()` twice per entry and now calls it
once (the position after entry N is the position before entry N+1, so it is
carried forward). That halved the calls and moved the ratio only from 1.75x to
1.63x, so the per-call cost of telldir is NOT the dominant term and the
remaining superlinearity is somewhere else -- most likely the per-entry
metadata lookup in the fakefs backing this measurement rather than the dirent
loop itself. Worth re-measuring against a realfs directory and a tmpfs one
separately before changing anything: the audit files this as two findings (a
quadratic telldir and an unindexed tmpfs directory scan) and the evidence so
far does not clearly implicate either.

### Address-space walks: what still costs per page or per region

**Established (2026-09-25).** The hole finder, and every walk built on "next
mapped page" / "next unmapped page", used to read the 56-byte entry of every
page it passed -- and leaves are immortal, so also every entry of every leaf a
process had ever used. One mmap(NULL, 4096) cost 11.8 ms beside a 2 GiB
MAP_SHARED memfd mapping and 10.4 ms after it was unmapped, against 0.02 ms
alone, and `dotnet --info` spent most of an hour there. Per-leaf occupancy
bitmaps with per-chunk used/full summaries (emu/memory.c, "occupancy bitmaps")
made both 0.012-0.014 ms; `tests/manual/mmap_hole_scaling` guards it, and
`ISH_PT_OCCUPANCY_CHECK=1` verifies every bit and `vm_entries` after each
structural change. Fault backpressure reads the resident-set counter instead of
walking, and RLIMIT_AS reads VmSize's counter.

**Still open, none of them measured as a problem yet:**
- `pt_find_hole` is O(occupied runs between mmap_floor and mmap_ceiling), each
  a few words. Linux is O(log n) with a gap-augmented VMA tree. An address
  space fragmented into thousands of separate runs would still pay per run.
- RLIMIT_DATA, when finite and the mapping is data, walks every mapped page's
  flags per mmap/brk/mremap (`vm_may_expand`). A data-page counter would need
  maintaining at every flags change, not just every entry change.
- `/proc/<pid>/maps` and `smaps` compare flags page by page, and
  `mem_resident_page_count` (VmSwap, `/proc/ish/swap_evict`) asks each mapped
  entry's frame. Both skip unmapped pages and empty leaves now, but are still
  linear in mapped pages.
- fork copies entries one page at a time; that is the page-table design, not a
  walk.

**Next step:** only if a workload shows one of these in a `sample`.

### What is still missing from procfs

Added 2026-09-01 alongside the procfs work in `tests/manual/proc_files.c`,
which closed /proc/{devices,partitions,swaps,modules,cgroups,interrupts,
thread-self,sysvipc/*}, /proc/sys/fs/inotify/*, procfs link counts, and
status's Umask/SigIgn/SigCgt plus stat's starttime. What is left, measured
against Linux 6.12:

**`/proc/locks` does not exist.** Linux lists every POSIX, OFD and FLOCK lock
with type, holder pid, major:minor:inode and byte range; `lslocks(8)` and
`lsof` read it and see nothing without it. AOK has all of this in `fs/lock.c`
-- the work is enumerating the per-inode lock lists safely from procfs, which
means a lock-ordering question (procfs read -> inode locks) rather than
missing data.

**`/proc/sys` is 29 keys against Linux's ~1355.** The audit called this out
and it stays a deliberate non-implementation: the keys that exist are the ones
something reads, and inventing the rest would mean 1300 files whose values are
made up. Adding a key is cheap when a real consumer turns up.

**A directory listed by a readdir callback reports nlink 2, not 2 + its
subdirectories.** /proc itself and /proc/<pid> are built by callback rather
than a static child table, and counting their subdirectories means walking the
pid table on every stat. Linux reports 219 for /proc and 9 for /proc/<pid>;
2 is the honest floor, and no longer 0, which is what a deleted inode looks
like.

**System V shared memory segments are not listed**: /proc/sysvipc/shm is its
header alone and shmctl has no SHM_STAT/SHM_INFO, so `ipcs -m` shows nothing
even while segments exist (kernel/ipc.c implements them). Semaphores and
message queues are listed for real.

**`/proc/meminfo`'s Shmem, AnonPages and Mapped are not Linux's figures.**
Since 2026-09-25 they are counters (`class_entries` in struct mem), so a read no
longer walks every page of every address space -- 0.5 ms alone and 54 ms beside
a 2 GiB memfd mapped twice before, 0.05-0.06 ms both after, 0.011 ms on Linux.
The counters kept the old walk's values exactly, and those differ from what
Linux means. Measured with the same program on amd64 and on Linux 6.12 (camd),
the change in kB as each step happens:

| step (16 MiB unless said)          | Linux 6.12                     | AOK                         |
|------------------------------------|--------------------------------|-----------------------------|
| private anon mapped, untouched     | 0                              | AnonPages +16384            |
| ...then touched                    | AnonPages +16384               | 0                           |
| shared anon mapped, untouched      | 0                              | Shmem +16384                |
| ...then touched                    | Shmem +16384, Mapped +16384    | 0                           |
| memfd mapped RW and RX, untouched  | 0                              | Mapped +32768               |
| ...touched through RW              | Shmem +16384, Mapped +16384    | 0                           |
| ...read through RX too             | 0                              | 0                           |
| ...unmapped, fd still open         | Mapped -16384 (Shmem stays)    | Mapped -32768               |
| ...fd closed                       | Shmem -16384                   | 0                           |
| file mapped private, then read     | Mapped + the pages not already mapped elsewhere | Mapped + all, at mmap |
| 2 GiB memfd mapped twice, untouched| 0                              | Mapped +4194304             |
| fork, child breaks half of 32 MiB  | AnonPages +16384               | AnonPages +32768            |

So AOK counts page-table entries per address space at mmap time; Linux counts
resident pages once each, from first touch: AnonPages is anonymous pages
mapped anywhere, Mapped is file pages (shmem included) mapped anywhere, and
Shmem is every shmem page -- memfd, shared anonymous, tmpfs, SysV -- mapped or
not. Linux-shaped figures need per-page state rather than per-entry: a
touched-and-first-mapping count on the frame (struct data already tracks its
owners), a memfd/shared-anon page counted as Shmem, and tmpfs file pages
counted whether mapped or not. `tests/manual/meminfo_scaling.c` asserts only
what both agree on; its table would change with this.

### PIPE_BUF atomicity cannot be imposed on a HOST pipe

Measured 2026-09-01. A write of at most PIPE_BUF is atomic on Linux: with
insufficient room it writes NOTHING and blocks or returns EAGAIN, rather than
putting in what fits. That is why several processes may share one pipe for log
lines -- a partial write splits a record and the next writer's bytes land in
the middle of it.

AOK's own FIFO buffer now honours it (`fs/fifo.c`, covered by
`tests/manual/fd_conventions.c`), which is every FIFO on a tmpfs. A `pipe(2)`
pair is different: `fs/pipe.c` hands the guest a HOST pipe and writes go
straight through `realfs_fdops`, so the guarantee is Darwin's, and Darwin's
PIPE_BUF is **512**, not 4096. A guest write between 513 and 4096 bytes can
come back short where Linux would have refused it whole -- measured directly
on the host: a 1024-byte write with ~600 bytes free returns 600.

Enforcing it from here needs the free space BEFORE the write, and Darwin does
not offer it. What it does offer, measured:

- `PIPE_BUF` is 512; capacity starts at 16K and grows to 64K on demand
- `fstat(fd).st_size` on EITHER end reports the bytes currently buffered
- `ioctl(FIONREAD)` works on the read end only; the write end answers 0
- there is no `FIONSPACE`, and no `F_GETPIPE_SZ`/`F_SETPIPE_SZ`

So the buffered count is available but the capacity is not, and free space is
capacity minus buffered. A running estimate of capacity would be a lower
bound, which makes the check refuse writes that had room -- wrong in the other
direction. The real fix is to stop delegating: give `pipe(2)` AOK's own
buffer, the way tmpfs FIFOs already have one. That is a large change (pipes
are handed to native code and to the host across `exec`), so it is recorded
rather than attempted here.

### Three file-mapping behaviours that need what the memory model does not keep

Measured against Linux 6.12 on 2026-09-01, alongside the mmap conformance work
that closed seven of the group (`tests/manual/mmap_conventions.c`). These three
were left because each needs state `emu/memory.c` does not carry, not because
the behaviour is in doubt.

**`MADV_DONTNEED` on a MAP_PRIVATE FILE mapping keeps the COW copy.** Linux
drops the private page so the next read comes back from the file: write 'Z'
over a file byte 'A' through a private mapping, `madvise(MADV_DONTNEED)`, read
again, and Linux gives 'A'. AOK gives 'Z'. `kernel/mmap.c` already handles the
anonymous case (jemalloc depends on it) and says so in a comment; the file case
needs the page's ORIGIN to still be reachable after the copy-on-write, and a
`struct pt_entry` that has been written keeps only the private copy. Doing it
properly means remembering the file-backed `struct data` per COW page.

**A load or store past EOF does not SIGBUS if its host page holds any of the
file.** Linux faults at guest-page granularity; the host pages a file in at its
own, 16 KiB on Apple silicon. So a guest page past EOF that shares a host page
with the file's last bytes reads zeroes and takes stores, which become file
content if the file later grows: guest pages 2 and 3 of a 5000-byte file read
as zeroes on alpine-arm64-test, and `write(2)` from one returns 16, where Linux
6.12 gives SIGBUS and EFAULT (measured 2026-09-25). A host page WHOLLY past
EOF does fault -- a guest SIGBUS from the JIT, and a failed syscall from the
kernel (next section) -- and a host whose page is the guest's has no gap. The
fault handler would have to know the backing file's current size at fault
time, which means carrying the file identity into the page fault path rather
than just the host memory.

**`remap_file_pages` is ENOSYS.** Linux has emulated it over mmap since 3.16
and a linear remap returns 0. Linux's emulation is a `MAP_FIXED` shared mapping
of the same backing over the subrange at the new offset. Lazy reservations are
no obstacle: every page-table entry already carries its own `data` and
`offset`, and a large shared anonymous mapping that is still reserved can be
materialised first, as `mprotect` does. What is missing is the syscall itself.

### File pages past EOF: what the guarded copies leave

Fixed 2026-09-25: kernel C code that touched a host page of a file mapping
holding no byte of the file -- `write()` from it, `read()` into it,
`/proc/<pid>/mem`, `process_vm_readv`, `ptrace(PEEK/POKE)`, a futex word, the
copy-on-write break after a fork -- took a host SIGBUS that ended the whole app
(exit 138). Every such access to a page that is not anonymous now runs under a
fault guard (emu/host_fault.h) that both host fault handlers resume at, so the
syscall fails as Linux 6.12's does: EFAULT, or EIO from `/proc/<pid>/mem` and
`ptrace`, and a store after a fork is a guest SIGBUS. (The forced accesses and
the copy-on-write break were closed first, by "mem: a debugger's write leaves
the page as protected as it was", through `mem_host_copy`, which now makes the
same guarded copy.) `tests/manual/syscall_page_past_eof.c` takes every route,
each beside a control inside the file. What is left:

**A `read(2)` that faults has already consumed its data.** AOK reads into a
kernel buffer and then copies it out, so when the copy faults the bytes are
already gone -- from a pipe, or past a file's offset. Linux copies first and
consumes only what it copied: after `read(pipe, bad, 16)` fails EFAULT, the next
read on Linux returns the 16 bytes, and on AOK it returns EAGAIN (measured on
camd and alpine-arm64-test). Any bad buffer does it, not only a page past EOF.

**On device, a store into the host page that holds EOF.** kernel/exec.c's
split_tail comment records APFS failing the copy-on-write page-in of a host page
that straddles EOF, on iOS. A syscall's copy into such a page is guarded like
any other (EFAULT) and the guest's own store is a guest SIGBUS, where Linux lets
a store into the guest page holding EOF succeed. Not reproduced on macOS 26,
where that store succeeds (host probe, 2026-09-25); unmeasured on a device.

**Debug-only readers are unguarded.** emu/amd64_interp.c's trace functions and
kernel/user.c's htop trace `memcpy` from `mem_ptr` directly; each runs only
behind its own `ISH_*` trace knob.

### FIXED 2026-10-01: `fcntl(F_GETFL)` on a pipe reported an O_NONBLOCK the guest never set

realfs_getflags now answers O_APPEND and O_NONBLOCK from fd->flags (see the F_GETFL entry above); tests/manual/fcntl_getfl_flags.c covers the get-set-restore idiom. The original diagnosis follows.

Found 2026-08-23 while writing `native_ptrace_group_stop.c`, whose drain step
did the textbook thing and got bitten:

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ... read until EAGAIN ...
    fcntl(fd, F_SETFL, flags);          // "restore"

The restore left the pipe NON-blocking, and every later `read` returned
EAGAIN. Not an `F_SETFL` bug -- `flags` was already `O_NONBLOCK` when it was
read back, so the restore faithfully wrote it again.

**Where the lie comes from.** A guest pipe is a host pipe (`fs/pipe.c` ->
`realfs_fdops`), and `realfs_getflags` (`fs/real.c:1160`) answers `F_GETFL` by
asking the HOST descriptor:

    int flags = fcntl(fd->real_fd, F_GETFL);

Meanwhile `realfs_read` (`fs/real.c:585`) permanently forces that host
descriptor non-blocking the first time the guest does a *blocking* read on it,
and deliberately never restores it -- the comment there is right about why
(restoring it races a sibling task into an uninterruptible, SIGKILL-proof host
`read`, which is a real pipeherd hang that was fixed by exactly this). So the
host flag is an implementation detail that must not be visible, and it is:
before the first read `F_GETFL` says 0, after it says `O_NONBLOCK`, with the
guest having done nothing.

The kernel's own `fd->flags` -- which is what actually governs guest blocking
semantics, and what `realfs_setflags` maintains -- still says blocking. The
two disagree and `F_GETFL` returns the wrong one.

**Next step.** `realfs_getflags` should report the guest-visible flags from
`fd->flags` for the bits the guest owns (`O_APPEND`, `O_NONBLOCK`) and take
only the access mode and the rest from the host. Small, but it needs its own
test: the get-modify-set idiom is everywhere, and silently turning a pipe
non-blocking under a program that never asked is the kind of thing that
surfaces far from here. Worth checking whether sockets and ttys answer
`F_GETFL` the same way before fixing just the one path.

### #523: yay's reported failure does not reproduce; an http2 flake does

Reproduced the environment on 2026-08-20 -- Arch Linux ARM aarch64, yay v13.0.1
built from AUR under emulation -- and ran `yay -S pandoc-bin` four times. **The
reported `context: signal: terminated` never appeared.** All four got through
the AUR fetch and downloaded sources.

Getting there needed five things fixed first, only one of them AOK's:

1. Landlock, 2. a dangling /etc/resolv.conf, 3. an empty keyring -- all three
   now shipped as `/AOK/fixes/arch`.
4. **`/dev/fd` was missing**, so bash process substitution was ENOENT and
   makepkg died at "Retrieving sources". Ours, and fixed (`59827f5ce`).
5. The minirootfs strips headers from 137 packages, so anything that compiles
   needs `pacman -S glibc linux-api-headers` first.

**What DOES reproduce, about one run in four:**

    request failed: Get "https://aur.archlinux.org/rpc?...":
        http2: client conn could not be established

yay recovers -- it falls back to git and carries on -- so it is not fatal, and
it is not what was reported. But it is a real intermittent failure of Go's
HTTP/2 client against a host that curl reaches every time, on both HTTP/2 and
HTTP/1.1. Ruled out already: not git (3 of 3 clones standalone), not TLS
generally (pacman syncs fine), not concurrency (9 simultaneous TLS operations
all succeeded).

**Measured 2026-08-20, and it is a latency tail, not a protocol bug.** TLS
handshake time to the same host, 15 samples each:

    host  (macOS)   min 0.09  median 0.21  max  1.15
    guest (AOK)     min 0.21  median 0.39  max 15.32

The median is about twice the host's -- unremarkable for emulation. The TAIL is
13x worse, and 15.3s is past Go's default TLSHandshakeTimeout of 10s, which is
exactly how "http2: client conn could not be established" arises. The same
stall explains the OpenSSL "unexpected eof while reading" seen from git.

**The CPU-count lie is not the cause**, though it was the obvious suspect:
AOK reports 4 of the host's 10 logical CPUs, and Go sizes GOMAXPROCS from it.
A Go HTTP/2 probe run at GOMAXPROCS 1, 4 and 8 (20 requests each) failed once
in 60, at GOMAXPROCS=8, with a handshake timeout -- the same tail, not a
scheduling effect.

**Next step.** Find what stalls a socket for seconds when the median is
sub-second. Nothing in the handshake is compute-heavy, so this is a wait that
is not being woken promptly rather than work that is slow -- which puts it in
the same neighbourhood as the poll/quiesce machinery. `curl` on its own shows
the tail too, so it reproduces without Go, without yay and without the AUR:
any repeated HTTPS handshake will do.

---

### FIXED: a checkpoint save before the guest booted crashed on a NULL list head

**Root-caused and fixed 2026-09-12.** Four device crashes, all
`task_snapshot_collect+172`, `ldur x28, [x24, #-0x8]`,
`KERN_INVALID_ADDRESS at 0xfffffffffffffff8`, always from the app's background
save.

**The cause.** `alive_pids_list` was a bare global (`kernel/task.c`), so it
started **NULL/NULL** and only became a valid empty list when
`become_first_process` called `list_init` (`kernel/init.c:304`). But
`list_for_each_entry` has no NULL check, so any walk before boot followed
`next` into 0 and faulted on the FIRST iteration. `ckpt_check_scope` does have
the right guard -- it refuses with "there is no guest running" when the
snapshot is empty -- but it has to call `task_snapshot_collect` to learn that,
and that call was the crash. The guard sat downstream of the fault.

**What made it reachable, and it was a regression of mine.** The app's
background save (`AppDelegate.m`, `ISHSuspendGuardEnterBackground`) is gated on
`shouldSuspendToDisk`, **not** on the guest being booted. The session-resume
picker added earlier the same day defers `ensureBooted` while it waits for an
answer (`TerminalViewController.m:464-467`). So once any session slot existed,
every launch deferred the boot, and backgrounding fired a save against a kernel
that had never come up. The first device run had no slot, booted normally, and
saved 20 tasks and 24 sockets without incident -- which is why it looked
intermittent.

**The fix** is one line: `alive_pids_list` (and
`tasks_pending_deletion_queue`, the only other bare global head) are now
`LIST_INITIALIZER`-initialised, so an empty list READS as empty from load.
`init.c`'s `list_init` stays -- re-initialising an empty list is a no-op.

**Measured, same tree and build dir, only the declaration differing:**

| binary | `ISH_CHECKPOINT_AFTER=0.001` x8 | result |
|--------|--------------------------------|--------|
| before | 8/8 **SIGSEGV** (rc 139)       | crash in the walk |
| after  | 0/8                            | `refused (-3) there is no guest running` |

The reproducer to keep: `ISH_CHECKPOINT_AFTER=<delay>:<path>` with a delay
short enough to beat the boot. Driving `/proc/ish/checkpoint` instead exercises
`checkpoint_save` from a guest task and cannot reach this at all -- which is
why ~20 earlier attempts found nothing.

**Still worth doing:** gate the background save on the guest actually being
booted, so it refuses cleanly rather than relying on the scope check; and the
`list_remove`/`list_for_each_entry` NULL class below.

### list_remove leaves a NULL node and the walk macro has no NULL check

**The class behind the alive_pids_list crash above, and it is wider than that
one list.** `list_remove` (util/list.h:69) sets a node's `next` and `prev` to
**NULL**; `list_init` leaves a node pointing at **itself**; and
`list_for_each_entry` terminates only on `&item->member != (list)`, with no
NULL check. So any node that is reachable from a list while holding a NULL
`next` faults the walk at the `container_of` subtraction --
`ldur x28, [x24, #-0x8]`, address `0xfffffffffffffff8`.

Measured spread in kernel/ and fs/:

- **94** bare `list_remove(` calls vs **14** `list_remove_safe(`.
- Unguarded `list_for_each_entry` walks per list head: `mounts` 19,
  `group->threads` 9, `pid->pgroup` 7, `alive_pids_list` 6, `sighand->queue` 4,
  `poll->poll_fds` 4, and a long tail.
- `list_for_each_entry_safe` caches `next` one step ahead but has the same
  termination test, so it is not immune either.

**Fixed so far: only the two `alive_pids_list` sites** (`task_unlink_locked`,
`kernel/exec.c`'s exec unlink), which now `list_init` after removing. That is
the list with an actual device crash report behind it.

**The class fix would be one line** -- have `list_remove` re-init instead of
NULLing -- but it changes the meaning of `list_null()` for 94 call sites, and
`list_empty()` treats NULL and self-pointing as the same thing while
`list_null()` does not. That needs its own audit of every `list_null` reader
before it is safe, which is why it was not bundled into a checkpoint fix.

**Next step.** Audit `list_null()` callers, then decide between the one-line
`list_remove` change and converting the remaining bare removes to a re-initing
form.

### The Desktops applet's default height under-counts its action buttons

Noticed 2026-09-12 while adding the Session button, and **pre-existing** -- left
alone rather than changed under an unrelated commit.

`ISHWorkspaceWorkspacesContentSize` (app/WorkspaceViewController.m) derives the
applet's preferred size from its contents, but carries a single
`actionsHeight` term (36pt phone / 44pt otherwise) while `_contentStack`
receives more than one action row in the classic style:

- **classic**: `_newWorkspaceButton`, `_closeHiddenButton`, `_sessionButton`,
  `listCard` -- three buttons, one term (plus the `sessionHeight` term added
  with the Session button, so the shortfall is the *second* of the two older
  buttons).
- **modern**: `layoutRow`, `_sessionButton`, `listCard` -- covered correctly by
  `actionsHeight` + `sessionHeight`.

The consequence is cosmetic: this is the preferred/fallback size for a window
the user can resize, so the applet opens a little shorter than its contents in
the classic style and the bottom button sits tight against the edge.

**Next step.** Replace the fixed `actionsHeight` with a count of the action
rows actually added, so the two styles cannot drift apart again -- the same
bug will recur the next time a button is added to one branch and not the
other.

### An effective uid of 0 counts as every capability, whatever the effective set says

`current_capable()` (kernel/getset.c) is `superuser() || <the bit in
cap_effective>`, and `superuser()` is an effective uid of 0. Linux asks the
effective set alone. The one place a test shows it is
`tests/manual/exec_setid_unsafe.sh`, row R7. Root drops `CAP_SYS_PTRACE` and
`CAP_SETUID` from its effective set, calls `PTRACE_TRACEME`, and execs a
binary that is set-user-ID to uid 1000:

- **Linux** refuses the new uid, since the tracer could not have attached and
  the caller cannot set ids itself. The image runs as 0/0/0/0.
- **AOK** runs it as 0/1000/1000/1000.

That is the only one of the test's 51 rows that differs.

Every privileged syscall asks the same function, so the fix is tree-wide. Each
`current_capable()` and `superuser()` caller needs checking against what Linux
asks there, and the capability tests need re-running as root with a reduced
effective set, which none of them do today.

Two more gaps in the same exec rules, neither with a test:
- **Shared filesystem context:** Linux's third unsafe-exec condition, a
  `CLONE_FS` shared with a process outside this one, is not modelled.
- **`#!` interpreters:** Linux honours an interpreter's own set-id bits, and
  AOK does not.

### Tracing a native program: what a tracer still cannot see

Found 2026-09-25 while fixing gdb's `startup-with-shell` under
`SHELL=/AOK/native/zsh` (tests/manual/ptrace_startup_with_shell.c). A traced
native program now reports its exec, and its own exec replaces it in place,
keeping its pid (native_exec_in_place_wanted, kernel/native.h) -- which is all
gdb's start-up needs. Three things a tracer sees on Linux it still does not:

- **No syscall stops.** A native program's calls go through
  syscall_dispatch_native, which has no ptrace hooks, and there is no guest
  register file to report them from. `strace` shows the exec and the exit and
  nothing between; a PTRACE_SYSCALL tracer sees one execve entry, the native
  program's exec events, and one exit.
- **Children are not followed.** A native shell starts a command with
  native_spawn, not clone, so `strace -f` and gdb's `follow-fork-mode` never
  attach to it.
- **An untraced exec still stands in.** Only a traced program's exec goes in
  place, since abandoning the program leaks its heap. Anything that attaches
  AFTER a native program has exec'd finds the stand-in's wait, not the program
  (its pid is the child's).

## Timers across a checkpoint

### FIXED: timers, and the signals they queue, were not in the image

**Fixed 2026-09-22.** POSIX timers (`timer_create`), the interval timers
(`setitimer`) and `alarm()` were not in a checkpoint image at all, so a
restored process that had armed one never got its signal: a program using
SIGALRM as a timeout waited for ever. Now each process's timers travel with its
first running task (struct group_timers_ckpt, kernel/timer_ckpt.h), are
rebuilt in their own slots, and are armed only once every restored task has
started -- `task_never_ran_destroy` does not free a group's timers, so arming
them any earlier would leave them firing into a failed restore.

- **Each deadline travels on the clock Linux counts it on**, not as "time
  left". MONOTONIC does not count the stop, and a relative arming on
  CLOCK_REALTIME and ITIMER_REAL live there (hrtimer_init moves them).
  BOOTTIME, the alarm clocks and a TIMER_ABSTIME arming on the wall clock do
  count it, and come due that much sooner. A CPU-time timer keeps the CPU time
  it had left. `posix_timer.abstime` and `fd->timerfd.abstime` remember the
  arming, and a timerfd now uses the same rule, where it was time left.
- **The signal queues travel too.** Only the task's `pending` bitmask was
  carried, and delivery takes from the queue lists while the waits ask the
  bitmask, so a signal pending at the save came back as a bit with no signal
  behind it: never delivered, and once unblocked, every wait returned EINTR at
  once (measured: a 0.2 s select took 0.000 s). Both queues now come back with
  their siginfo, overrun counts included, and the pending sets are rebuilt from
  them. Timers are read before the queues, so a timer firing mid-save costs at
  most an overrun, never an expiry.
- `signal_wake_task` no longer pokes a task whose host thread has not started
  (its `thread` is the parent's, or nothing, for the app's pid 1 before
  `task_start`), which a due timer made ordinary on the resume path.

**Three defects in the first version, found by an adversarial review and each
proved on the committed binary (e28c9476) before its fix:**
- *ITIMER_VIRTUAL/PROF were measured on the wrong thread.* `cpu_time_now_of`
  asks the CALLING thread for whichever member is `current`, and the writer
  sets `current` to the task it describes. With 2.5 s of CPU on the carrying
  thread, ITIMER_PROF came back 2.6 s of CPU late. The group's CPU is now read
  with `current` cleared (`group_cpu_now`), on both sides.
- *An expiry being delivered during the save was lost.* The timer thread
  decides to fire, drops its lock and only then queues the signal; a timer read
  in that gap was a fired one-shot, and its signal was not in the queue yet.
  `timer_read` now waits the delivery out, and ITIMER_VIRTUAL/PROF are re-read
  if the sampler ticked meanwhile (`timer_settle`). The window is microseconds,
  so `ISH_TEST_TIMER_FIRE_DELAY_MS` holds every delivery open: with it, the
  committed code lost both a POSIX timer's and ITIMER_REAL's signal 3/3, and
  the fix delivered both 3/3 (checkpoint_timers.sh's race leg).
- *"Never" overflowed.* A timer or timerfd armed at TIME_T_MAX -- systemd's
  clock-change watch -- and a nanosleep of TIME_T_MAX, which is what
  `sleep infinity` asks for, came back due at once: the nanosecond arithmetic
  wrapped. Saturated now (TIMER_CKPT_NEVER), and "never" comes back never.

**Test:** `tests/manual/checkpoint_timers.sh [root]` (+ .c), both save paths, a
3 s stop: alarm() in a child, POSIX timers on MONOTONIC, REALTIME relative
and absolute, BOOTTIME, periodic, SIGEV_NONE, process and thread CPU clocks,
ITIMER_REAL and ITIMER_PROF, queued signals with their siginfo, and the sleeps
below. Before the fix every check failed on both legs (24 on x86_64); after, each meets its
deadline within ~10 ms, on devuan amd64 and arm64 (glibc) and alpine amd64
(musl).

### FIXED: a sleep the freeze interrupted started over -- even with no restore

**Fixed 2026-09-22.** The earlier entry here said that within one process the
freeze's restart carried the deadline. It did not: a freeze reached a sleep, poll or
select as a bare EINTR, which `syscall_result_should_restart` restarted, but
nothing had recorded the deadline, so the re-executed call waited its whole
timeout again. Measured on the old binary with a plain save, no restore:
nanosleep, clock_nanosleep, select, pselect6, poll, ppoll, epoll_wait and
epoll_pwait all took 8.07 s for a 6 s timeout. Across a restore `sleep 5`,
frozen 3 s in, slept 5 s more.

Now the sleeps (`sleep_restart_or_eintr`) and `poll_wait` report a freeze as
the `_ERESTART_NOHAND` it is and keep their deadline, as a job-control stop
already did. The image carries it on the guest clock (MONOTONIC; BOOTTIME for a
relative BOOTTIME sleep, which counts the stop), with the pending-rewind flags,
so a handler that runs before the call re-executes still cancels it. epoll
keeps a freeze's restart and drops the deadline with every other one: nothing
consumed it, so after a SIGSTOP the NEXT poll or select to run waited out the
stale deadline, or epoll's own 2 s cap. `tests/manual/checkpoint_freeze_restart.c`
now also fails a sleep or poll-family case that returns late.

### Timers and timed waits: what is still open

- **Every other timed wait still starts its timeout over after a freeze:** a
  relative futex FUTEX_WAIT (and so every glibc timed lock and condvar wait
  that is relative), `rt_sigtimedwait`, `semtimedop`, a socket's
  SO_RCVTIMEO/SO_SNDTIMEO wait, and clock_nanosleep on a CPU clock. Each needs
  a deadline carried the way `sleep_restart_deadline` carries one (Linux's
  restart_block). kernel/calls.c's `syscall_result_should_restart` names them.
  A relative FUTEX_WAIT does now keep its deadline across a restart nothing
  ran in front of -- a stop, an ignored signal (`futex_restart_deadline`,
  parked with the wait) -- but a freeze answers "no restart" from the signal
  side, which drops the park, so it still starts over there.
- **The CPU-time clocks start again from zero after a restore.** A restored
  thread is a new host thread, so CLOCK_PROCESS_CPUTIME_ID,
  CLOCK_THREAD_CPUTIME_ID, getrusage, times() and /proc/<pid>/stat's
  utime/stime all go backward across one. A CPU-time timer is right -- it
  carries the CPU time it had left -- but a reading taken before the save, or
  an absolute CPU-clock arming made from one, is not.
- **The clocks resume when the restore STARTS** (`guest_clock_resume`), so
  MONOTONIC counts the restore's own duration, which Linux's does not -- it
  continues from the thaw. Every carried deadline agrees with it, so a relative
  and an absolute wait still agree; moving the resume to just before the thaw
  would fix all of them at once.
- **A native program's pending signals come back as bits with no queue entry**,
  as before. It is re-launched, and nothing it had pending is delivered.
- **Found alongside, not a checkpoint bug:** `ppoll` with an INT64_MAX
  timeout returns 0 at once (measured on the pre-change binary, no checkpoint
  involved), where nanosleep and clock_nanosleep with the same value sleep --
  poll_wait's deadline arithmetic overflows. A NULL timeout is the usual way to
  say "for ever", so nothing common hits it.
- **Found alongside, not a checkpoint bug -- FIXED 2026-09-23:** a signal
  whose delivery runs no handler (SIGCHLD with SIG_DFL) ended a restartable
  wait with EINTR, where Linux restarts the call; and
  `deliver_signal_to_group_locked` queued such a signal whenever ANY member
  blocked it, where Linux asks only the target. musl's fork() and pthread_exit
  block every signal, so a child dying while a sibling thread exited EINTR'd
  the other threads' sleeps. Now only the target's mask decides, and an ignored
  signal restarts what it interrupts, even when a sibling took it first
  (kernel/signal.c; `tests/manual/signal_ignored_restart.c`). A HANDLED
  process signal woke every thread too, so its handler ran in a sibling and
  other siblings' calls failed with EINTR; since the same day ONE thread is
  told, as Linux's complete_signal does
  (`tests/manual/signal_process_wake_one.c`).
  `checkpoint_timers.c` still parks its threads and its asker, so it depends
  on neither.

## Suspend and resume across the three modes

The goal: a suspend or checkpoint comes back exactly as it was, whether it was
taken in shell, Workspace or Wayland mode, with every applet in it (the Wayland
display included). Terminal and applet restoration are done. These three are
open. All were reported 2026-09-15.

### Saving takes a long time when a Wayland applet is open

**Established.** Nothing yet. The user reported that a suspend with a Wayland
applet open is slow on the SAVE side. It has not been measured and the cause is
not known. Candidates, none checked:
- the freezer waiting for the compositor's or RFB client's tasks to park, up to
  the freeze timeout in kernel/checkpoint.c;
- a much larger image, because a Wayland session keeps framebuffers in guest
  memory;
- the app side of the save (ISHWorkspaceCaptureLayoutForSuspend, and
  DisplayViewController tearing down its RFB connection).

**Next step.** Time the phases. Save the same session with and without the
Wayland applet open, and compare the image size, the `session.*` breadcrumb
timestamps in Diagnostics, and an `ISH_CHECKPOINT_DEBUG` trace of which task
the freezer waits on.

### Restore should come back in the mode it was saved in

**Established.**
- The launch mode comes only from the Settings "Initial Window" preference.
  SceneDelegate.m reads it through `ISHShouldLaunchWaylandDisplayAtStartup()`
  and `ISHShouldLaunchWorkspaceAtStartup()`.
- A suspend records the Workspace arrangement next to its image
  (`ISHWorkspaceCaptureLayoutForSuspend`). It does not record which mode was on
  screen.
- So a session saved in Wayland mode and resumed with the preference set to
  Workspace comes back in Workspace, and the reverse.

**Next step.** Record the on-screen mode (shell, Workspace, or standalone
Wayland) beside the image, the same way the layout is filed. On a resume,
choose the window from that record instead of the preference, and use the
preference only for a fresh boot.

### Wayland mode has no quick way to suspend or checkpoint

**Established.** Shell mode has a Save Session button and a Cmd+S key command
(TerminalViewController.m, TerminalView.m). Workspace has "Save Session" in its
root menu (WorkspaceViewController.m). The standalone Wayland display
(DisplayViewController) has neither. Its only suspend path is backgrounding the
app with Suspend to Disk on.

**Next step.** Give the standalone display the same two actions the others
have, "Save Session Now" and "Suspend and Exit" (`ISHSuspendSessionSaveNow`,
`ISHSuspendSessionSuspendAndExit`). Offer them as an on-screen control that
does not cover the desktop, plus a key command. Cmd+S matches shell mode and
looks free there: DisplayRFBView forwards only Cmd+= + - 0 to the guest, and
deliberately not Cmd+letter. Confirm it does not also reach the Wayland
session before claiming it.

### The signal waits can still be poked into EINTR

**Established (2026-09-17).** The address-space barrier
(`task_poke_shared_mem`) skips a task that is `io_block`, but it can read the
flag a moment before a task entering a blocking call sets it. The poke then
lands inside the wait, and `wait_for` reports it as `_EINTR` with no signal
pending. Seen unforced once in several hundred tries: an `inotify` read on the
Devuan arm64 root came back EINTR at 0ms with no handler run, while a sibling
thread mapped memory. `ISH_TEST_POKE_BLOCKED_TASKS=<comm prefix>` forces the
race (kernel/task.c).

Forced, it failed every wait that trusted `io_block` alone, and all of those
outside kernel/signal.c are now on `wait_for_blocked` (util/sync.c), which
treats a bare poke as a spurious wakeup: `eventfd`, `inotify` and `timerfd`
reads, pty reads and writes, FIFO opens, reads and writes (tmpfs FIFOs in
fs/fifo.c; a host FIFO's open retries in fs/real.c), `F_SETLKW`, `flock`, the
kmsg wait and the `/dev/fuse` read. `tests/manual/blocked_wait_state.c` checks
thirteen of them, and run with the knob it failed all thirteen before the
change and passes after.

**Still open: pause, rt_sigsuspend, rt_sigtimedwait and the signalfd read**, all
in kernel/signal.c, which was being rewritten for the restart record when the
rest landed. Forced, `rt_sigtimedwait` returned EINTR within milliseconds.
Only signalfd can take `wait_for_blocked` as it is, because its loop re-reads
the signals first. The other three cannot:
- `rt_sigtimedwait` waits for signals it has BLOCKED. Their arrival shows up
  only as the interruption mark a poke also leaves, which
  `task_wake_signal_pending` does not count. Its loop is
  `do wait_for(...) while (err == 0)`, with no look at the set, so a spurious
  wakeup would wait on past the signal. It needs to check the set on every
  pass first.
- pause and rt_sigsuspend loop until `wait_for` says `_EINTR`, so they need the
  same predicate inside the loop rather than a different wait.

**Next step.** Once the restart-record work in kernel/signal.c has landed,
restructure those loops as above. Then add the four calls to
`blocked_wait_state`'s knob-driven checks.

### A task blocked OPENING a FIFO still cannot be frozen if its wake is lost

**Established (2026-09-15).** The freezer's wakes (a `pthread_kill` and a
cond notify) can be lost on a device. `ISH_CHECKPOINT_LOSE_WAKES=1` drops them
on the CLI, so that failure can be reproduced on a Mac. Every cond-based wait
in the kernel now checks for a freeze once a second (`wait_for` in
util/sync.c). With the wakes dropped, a sweep of blocking shapes shows:
- **Now freeze:** dash/busybox `wait` (rt_sigsuspend), bash `wait` and
  `waitpid` (wait4), `flock`, a pipe read, and perl `pause`/`sigsuspend`.
- **Still refuses:** `cat` opening a FIFO nobody has opened for writing:
  "did not reach a syscall boundary (blocked in arm64 syscall 56)".

A fakefs FIFO is a real host FIFO, and `realfs_open` calls the host `openat`
without O_NONBLOCK. The task therefore sits in a HOST syscall that only the
`pthread_kill` can interrupt. No wait slice helps, because the task is not in a
cond wait.

**Why it was not fixed with the rest.**
- The writer half is simple: open O_WRONLY|O_NONBLOCK, and retry on ENXIO in
  short slices that ask about signals and the freeze.
- The reader half has no faithful emulation. A non-blocking O_RDONLY open
  succeeds at once, and Darwin offers no way to ask whether a writer exists.
  Inferring it from Darwin's spurious POLLHUP misses a writer that opens and
  closes without writing. Linux wakes the blocked reader for that writer, so
  the inference would change guest-visible behaviour.
- Cancelling a blocked reader by briefly opening the write end is visible to
  any other reader of the same FIFO, which would see a writer come and go.

**Next step.** Do the writer half as above. For the reader, look for a Darwin
query that reports the writer count, or accept the POLLHUP inference only
where a lost write-and-close cannot happen. Until then this is a
rarely-hit shape: a checkpoint has to land while a process sits in an unpaired
FIFO open.

## Deferred on purpose

### Suspend and Exit terminates the app, which the HIG discourages

`ISHSuspendSessionSuspendAndExit` ends in `exit(0)`, because iOS has no public
"quit my app" API and the feature is, precisely, to put the machine down and
leave. It is behind an explicit, confirmed, user-initiated action in the Session
menu -- never automatic -- and the image is fsynced and renamed into place
before the process goes, so the session is durable rather than merely written.

**The decision to make before an App Store build**: keep it, or replace the exit
with a "session suspended" screen that leaves closing the app to the person.
Reviewers object to apps that appear to crash; an app that quits on a button the
user just confirmed is a weaker case against, but it is not no case. Recorded
here so the choice is made deliberately at submission rather than discovered.
`/AOK/tools/suspend.sh` has always ended the same way and is unaffected either
way, since a guest-initiated halt is not the app terminating itself.



### External display / AirPlay -- GH #540

Work exists on the branch `worktree-external-display-540`:

    6156597ee app: mirror the Wayland display to an external display (GH #540)

**Deferred to a future release by the maintainer (2026-08-18): "the external
display work is flawed".** The commit is NOT merged and must not be swept into a
release by accident. Left on its branch deliberately.

---

## Host capabilities worth exposing

What the iPhone and iPad hardware actually lets an app reach, and which parts
are worth surfacing to the guest. Surveyed 2026-09-07.

### Bluetooth LE -- the one radio that is genuinely open

**Established.** CoreBluetooth's central role is available to every app with no
MFi programme, no Apple-granted entitlement and no vendor agreement: scan,
connect, discover services and characteristics, read/write/notify against any
BLE peripheral. The peripheral role is open too, and `CBL2CAPChannel` (iOS 11+)
gives a real bidirectional stream over LE credit-based flow control rather than
characteristic ping-pong. Entry cost is one Info.plist key
(`NSBluetoothAlwaysUsageDescription`) and a user prompt.

Classic BR/EDR is closed in the other direction -- no RFCOMM, no SPP, no SDP,
no HCI, no programmatic pairing. Bluetooth serial needs MFi through
ExternalAccessory, the same gate as the port. Keyboards (HID) and audio (A2DP)
are handled by the system and already work in AOK for free. Multipeer
Connectivity and `Network.framework` peer-to-peer do use Bluetooth, but only
between Apple devices. AccessorySetupKit (iOS 18+) narrows the permission
prompt; it does not add capability.

One hard limit: there are no Bluetooth addresses. CoreBluetooth hands out an
opaque per-app UUID that differs between apps and rotates, so anything
`hcitool`-shaped is impossible by construction. Throughput is kilobytes per
second -- fine for sensors and control, useless for bulk transfer.

**Next step** is a `/dev/bluetooth` character device on the `/dev/url` recipe
([[dev-url-scheme-device]]), central role only to start: a line protocol for
scan / connect / read / write / subscribe, plus a guest-side helper tool.
app/LocationDevice.m is 190 lines for a read-only device; this one is read/write
and stateful, so budget several hundred, plus the same five registration points.

Two things to get right:

- **Do not fake BlueZ.** `AF_BLUETOOTH`, HCI sockets and `bluetoothctl` would
  mean synthesising controller-level events out of a GATT-level API, and the
  result reports states no real controller produces -- exactly the failure in
  [[capability-lies-are-load-bearing]]. Ship an AOK-native interface and
  document it as one. A `bleak` backend on top is a reasonable follow-on.
- **App Review will ask why a terminal wants Bluetooth.** The precedent is
  already in the tree: AOK ships `/dev/location` and asks for location
  permission on the same argument -- an opt-in device capability surfaced to a
  scripting environment.

### The Lightning / USB-C port -- mostly nothing to do

**Established.** There is no raw USB access at any tier: no enumeration, no bulk
transfers, no device nodes. What exists, in order of reachability:

- **Free, no code.** USB Ethernet adapters (just a network interface -- the
  wired device-testing link already rides this), USB and Bluetooth keyboards,
  USB audio, external displays.
- **Files plus security-scoped bookmarks.** iOS 13+ mounts USB storage into
  Files, and a document picker can select a folder on it. **AOK already has
  this** -- `iosfs` in app/iOSFS.m is exactly that mechanism. Limits: no block
  device, no `mount(2)`, FAT/exFAT/APFS/HFS+ only, and bookmarks go stale on
  unplug.
- **ExternalAccessory (MFi).** Per-accessory protocol strings declared in
  Info.plist, and the accessory needs Apple's auth chip. Not generic USB.
- **DriverKit (iPadOS 16+, M-series only).** The
  `com.apple.developer.driverkit.*` entitlements are granted by Apple per app
  against a specific hardware justification, and they vanish in unsigned builds
  ([[unsigned-ipa-drops-entitlements]]).

Thunderbolt is not a separate thing to expose: the M-series iPad port is USB4,
but there is no PCIe API on iPadOS. The device side is fully closed -- AOK
cannot present itself to a connected Mac as USB serial or mass storage.

**Verdict: no DriverKit work.** M-series iPad only, an entitlement Apple is
unlikely to grant for this use case, and dead in sideloaded builds -- it would
split the user base for a feature most users cannot run.

**Next step**, if anything: polish `iosfs` for external volumes -- a sane story
when the drive is unplugged mid-session and the bookmark goes stale, and a line
in the docs saying a USB-C SSD can be mounted. Users do not know AOK already
does this. A Redpark-backed `/dev/ttyUSB0` over ExternalAccessory would slot
into the same dyndev recipe and is the one genuinely differentiating port
feature -- iPad plus console cable -- but it needs specific hardware, Redpark's
licensing terms, and it is dead code for everyone without the cable. Only worth
it if the maintainer wants it personally.

### YubiKey for ssh -- possible add, tested by the requester

Asked for on Discord (2026-09-21) by a user who authenticates ssh with a
YubiKey over NFC or USB. They had tried to build it themselves and stopped at
the NFC entitlement, which a free Apple account cannot hold. That wall is ours,
not theirs: the TestFlight build is signed on a paid team, so an entitlement we
add reaches TestFlight users. It does not reach free-account sideloaders of the
GitHub IPA, whose re-signer drops what their team cannot hold
([[unsigned-ipa-drops-entitlements]]).

**Established.**

- **Shape: an ssh-agent in the app, not key passthrough.** The guest has no
  USB, HID or PC/SC, and building any of them is far more work than the agent
  protocol, which is small (list identities, sign) and stable. The app talks to
  the key; the guest gets a unix socket and `SSH_AUTH_SOCK`; guest `ssh`, `git`,
  `scp` and `rsync` work unchanged on every arch, musl and glibc.
- **Transports.** NFC is iPhone only -- iPads and Macs have no reader CoreNFC
  can use. USB-C (iPad, iPhone 15+, iOS 16+) goes through CryptoTokenKit and
  reaches only the key's smart-card applets: PIV, not FIDO2, because apps get no
  HID. Lightning (5Ci) is MFi ExternalAccessory, and App Store use needs Yubico
  to register the app -- skip it.
- **Key types.** PIV (slot 9a) works over NFC and USB-C. FIDO2 `ed25519-sk` /
  `ecdsa-sk` works over NFC only, and only by speaking CTAP2 to the key
  directly with its `ssh:` application id. Apple's AuthenticationServices
  security-key API cannot do it: it signs only for an associated web domain, so
  it never matches a key `ssh-keygen -t ed25519-sk` made. Existing `id_*_sk`
  files would load through `ssh-add`; creating sk keys on the device is out of
  scope.
- **Socket plumbing is small.** `unix_socket_get` (fs/sock.c) already maps a
  guest socket node to a host socket by `socket_id`, so the kernel makes the
  node and the app owns the host listener behind it. The listener must strip
  the 8-byte peer cookie AOK's connect sends first (the unix peer-token
  registry, same file). AF_UNIX listeners survive suspend
  ([[listening-sockets-die-on-suspend]]), so no resume hook should be needed --
  confirm it on the device rather than assume.
- **UX limits.** Signing needs AOK in the foreground (NFC sheets and PIN
  prompts both do). Over NFC it is one tap per ssh connection, so `git` work
  that opens many wants `ControlMaster`; USB-C has no such cost. Listing
  identities would cost a tap of its own unless the public keys are cached by a
  one-time "enroll key" step in Settings.

**Sizing** (estimated 2026-09-22, nothing built): about 2,500 lines. Agent
protocol and signature encoding ~700 lines of C; PIV and both transports ~600,
or far fewer on YubiKit (ObjC, Apache-2 -- compatible with GPLv3 -- vendored
as an emkey1 fork per [[vendor-as-fork-submodule]]), which also covers FIDO2;
app UI ~500 (enroll, key list, PIN prompt with an optional per-session cache).
FIDO2 sk keys over NFC are a second phase of ~500 more. Entitlements:
`com.apple.developer.nfc.readersession.formats` (TAG), the PIV and FIDO AIDs in
`com.apple.developer.nfc.readersession.iso7816.select-identifiers`,
`NFCReaderUsageDescription`, and `com.apple.security.smartcard` for USB-C.
NFC Tag Reading must be enabled on the App ID in the developer portal and the
profiles regenerated; whether the smart-card entitlement needs the same is not
known.

**Testing without hardware.** The maintainer has no YubiKey and does not plan
to buy one, and the simulator has neither NFC nor smart cards, so the
transports can only be proven by the requester on TestFlight. What can be
proven here first: the agent core and socket on the CLI build, behind a
software key backend, with guest `ssh-add -L` and `ssh` against a guest sshd.
Ship the hardware path behind a Settings toggle marked experimental, and log
each APDU exchange's status word with `ish_printk` so the tester can paste
`dmesg` back -- ssh itself only ever sees `SSH_AGENT_FAILURE`.

**Next step** is a question to the requester, not code: PIV or FIDO2? Yubico's
own guide calls FIDO2 the simplest ssh setup, and if that is what they use, a
PIV-first phase does nothing for them and the order flips.

### x86 guests have no crypto acceleration, and it costs 18-46x

**Established, measured 2026-09-18** ([docs/guest_pc_sampling_2026_09.md](guest_pc_sampling_2026_09.md)).
`jit/guest-arm64/crypto.S` maps the arm64 guest's AESE/AESMC/PMULL/SHA256H onto
the host's own crypto instructions and `kernel/exec.c` advertises them in
`AT_HWCAP`. The x86 guests get none of it, and `openssl speed` at 16 KB blocks
shows what that is worth:

| | arm64 guest | amd64 guest | ratio |
|---|---:|---:|---:|
| AES-128-GCM | 97,352 kB/s | 5,308 kB/s | **18.3x** |
| SHA256 | 109,685 kB/s | 2,383 kB/s | **46.0x** |

Not a benchmark artefact: on `apt install` this is 20.6% of on-CPU time on the
amd64 guest (`libmd` 10.6% + `libcrypto` 10.0%, hashing and verifying packages;
24.6% of wall) against ~1% on arm64, and 35.8% of on-CPU on amd64 `apk add` against 6.9%.

(Superseded 2026-10-08: the vector switch is on, and AES-NI and PCLMULQDQ are advertised with it, legacy SSE encodings included.)

**The scoping point that makes this tractable.** In `emu/cpuid.h` the AES-NI and
PCLMULQDQ bits sit inside `#if CPUID_ADVERTISE_VECTOR_STATE`, next to
XSAVE/OSXSAVE/AVX. That switch is 0 for a real reason -- the signal frame cannot
carry `ymm_hi`, so advertising AVX would corrupt registers across a signal.
**AES-NI does not share that debt**: its legacy SSE encodings use `xmm0-15`,
which the existing 512-byte FXSAVE signal frame already saves in full. The bits
are bundled there because making OpenSSL emit those encodings is, in that
comment's words, a separate claim needing its own evidence -- not because XSAVE
is required.

**Next step**: implement the legacy SSE `AESENC`/`AESENCLAST`/`AESDEC`/
`AESDECLAST`/`AESIMC`/`AESKEYGENASSIST` and `PCLMULQDQ` in the i386 and amd64
engines, mapped onto host AESE/AESMC/PMULL as `jit/guest-arm64/crypto.S`
already does, then advertise **only** bits 1 and 25 and leave
`CPUID_ADVERTISE_VECTOR_STATE` at 0. Do SHA256 in the same pass -- it is the
larger half of the loss, and the host instructions for it are already in use by
the arm64 guest. Note `emu/avx.c`'s `avx_aes_round` is a software S-box today,
so the VEX forms need the same treatment or they become a trap for anything
that probes AES-NI and then uses the VEX encoding.

Related and already shipped: `ISH_SYS_AEAD` (`kernel/ish_accel_aes.c` plus the
OpenSSL provider in `opt/AOK/tools/crypto`) is ABI-neutral, so it already works
for x86 guests -- but it is off by default, needs a provider installed in the
root, and covers AEAD ciphers only, not the bare SHA256 that `libmd` and `apt`
spend their time in.

---

## Native program candidates

Programs worth compiling in as native code (kernel/native.h), and the one
question that decides most of them.

**The dividing line is the shim, not the program.** kernel/native_libc.h works
by `#define`-ing libc names ahead of the system headers, so it redirects calls
in translation units AOK COMPILES. It does nothing to calls made from a
prebuilt dylib or from another toolchain's objects -- that is exactly what made
zlib's gz* family unusable until deps/smallclue-shim/zlib.h reimplemented it.
So candidates fall into two groups, and the second is a different project from
the first:

- **C sources AOK can compile itself.** bash, zsh, OpenSSH and nextvi are
  already here. Cost is the porting work the gate enumerates
  (`tools/check-native-libc.py --report <objects>`), which is finite and
  visible up front.
- **Anything built by a foreign toolchain** -- Rust, Go, or a vendored build
  system we do not drive. Their `open`/`read`/`write` resolve to the host's at
  link time and no `#define` reaches them.

  **Prototyped 2026-08-20, and it works with link flags alone.** Darwin's
  linker will alias an undefined import onto a symbol we define:

      clang -o prog shim.o foreign.o \
          -Wl,-alias,_nlibc_open,_open \
          -Wl,-alias,_nlibc_read,_read

  An object compiled with no knowledge of AOK then calls `nlibc_*` instead --
  verified against a control build of the same object, which read the host's
  real /etc/hosts while the aliased one read the stand-in guest VFS.
  `llvm-objcopy --redefine-sym` also works, including on static archives (what
  a Rust staticlib ships as), but it rewrites the objects and the alias needs
  nothing but the link line. Caveats and the one open decision are below.

### helix

Requested 2026-08-20. Modal editor, Rust, MPL-2.0 (confirm before any work --
the licence matters here the way GPLv3 does for bash, which is why
`-Dnative_bash` exists at all).

Second group, so it is behind the interposition question above. Beyond that:

- Rust std does its own syscalls, and helix does file I/O throughout -- there
  is no pure/impure split to exploit the way zlib's deflate/inflate allowed.
- LSP servers and formatters are spawned processes. `fork()` is ENOSYS for a
  native program, but exec/spawn works (see [[native-exec-standin]]), so this
  is probably not the blocker it first looks like.
- Tree-sitter grammars are built as loadable objects by default; a static
  grammar build would be needed.
- Size is tens of MB with grammars, against a binary that ships in an app.

AOK already has nextvi and micro native, so this is the "modern editor" slot
rather than a gap. **Next step** is the interposition prototype, not helix
itself -- pick the smallest Rust program that does one `open` and see whether
its objects can be made to call `nlibc_open`.

### gzip is already native, and nothing routes to it

**Established, measured 2026-09-18** ([docs/guest_pc_sampling_2026_09.md](guest_pc_sampling_2026_09.md)).
`tar xzf` spends 50-83% of its on-CPU time in a decompressor **binary**, never
in libz: `/usr/bin/gzip` on the Devuan roots, `/bin/busybox` on Alpine. Read off
the ELF, `DT_NEEDED` for `/usr/bin/gzip` is `libc.so.6` alone -- GNU gzip and
busybox each carry their own inflate, so no libz accelerator of any kind can
touch this workload.

smallclue already has `gzip`/`gunzip`/`zcat` applets. Shadowing the guest's
gzip with the native one on PATH, interleaved A/B, median of 4, same 14 MB
tarball:

| guest | distro gzip | native gzip | speedup |
|---|---:|---:|---:|
| amd64 / glibc | 13.130 s | 4.759 s | **2.76x** |
| arm64 / glibc | 5.583 s | 4.797 s | **1.16x** |

So the capability exists and is unreached, because `/AOK/tools/native-links.sh`
is not applied by default. **Next step** is a decision, not code: whether
shadowing a distro binary by default is acceptable (it changes what `tar -z`
runs, and the applets' flag coverage would need to be checked against GNU
gzip's first). The arm64 win is smaller because what is left there is tar's own
file creation through fakefs, not the codec.

### smallclue's `git`, `rsync` and `dvtm` are compiled as stubs

**Wanted, and deferred on 2026-09-25: not before 556.** smallclue carries all
three, and the applets are in AOK's binary, but each one only refuses:

| applet | what it says today | why |
|---|---|---|
| `git` | "libgit2 support is not enabled in this build" | built without `PSCAL_HAS_LIBGIT2` |
| `dvtm` | "applet is disabled in this build" | built without `SMALLCLUE_WITH_DVTM` |
| `rsync` | "openrsync: not built into this iSH-AOK" | AOK's own stub, `kernel/smallclue_glue.c` |

meson.build takes only openssh and nextvi from deps/smallclue/third-party. So
AOK has never checked out the dvtm, libgit2 and openrsync submodules, not even
in the main checkout. Each one is a port, not a flag:

- **libgit2** (`git`). It has to be built for iOS inside meson, with every
  source file going through kernel/native_libc.h like the rest of smallclue.
  - It needs a TLS and HTTPS transport, and AOK links no OpenSSL. The
    NSURLSession-backed curl shim in deps/smallclue-shim is the likely route,
    through a custom smart-HTTP transport.
  - ssh remotes need an ssh transport; running the native `ssh` is one option.
  - Licence: GPLv2 with the linking exception. Check it against the
    Licensing summary in meson.build before shipping. Nothing else in the
    binary is GPL unless `-Dnative_bash` is on.
- **dvtm**. It starts a shell on each pty, and a native program cannot
  `fork()`. That needs AOK's native spawn with a pty.
  deps/smallclue/src/dvtm_runtime_hooks.h is smallclue's iOS hook point.
- **openrsync** (`rsync`). Its remote side runs over an ssh child. That needs
  native spawn plus pipes, and deps/smallclue/src/openrsync_ios_shim.h and
  openrsync_hooks.h are the starting points.
- **Submodules.** Initialise them in the main checkout only, never from a
  worktree, because worktrees share the submodule git dir. A worktree made
  now gets only the two submodules the build uses, and git then marks
  deps/smallclue as modified for the three missing folders. Creating the
  folders silences it.

Until then, the guest's packages do the job: `apk add git rsync dvtm`, or
`apt install` on Devuan. They are slower, being translated rather than native.

### Library-level native interposition: measured, and the answer is no

**Established 2026-09-18.** The idea of running a host libz/libcrypto/libc in
place of the guest's was measured with a sampling profiler
(`kernel/guestprof.c`, `ISH_GUEST_PROFILE`) across four workloads and four
guests. It does not pay:

- `tar xzf` reaches no shared library at all (above).
- In `git clone`, libz is 8-11% of on-CPU against `[kernel]` at 22-38%.
- The only library clearing a high bar is `liblzma` (27-40% of on-CPU in
  `apt install`, because `.deb` data is xz) -- and on musl that slot is libz
  instead, because `.apk` is gzip. Even "accelerate the package manager's
  codec" is a different library per distro, for a win confined to one of them.

No further work is planned on it. The instrument stays; the full argument,
including what would have had to be true for a go, is in the doc.

---

## Large work in flight, tracked in its own document

Recorded here because this file was silent about the biggest thing in the tree
for a whole cycle. These are not TODO entries -- each has a plan document that
is the live record -- but a reader who does not know they exist will misread
everything above.

**Simulated swap.** [docs/simulated_swap_plan.md](simulated_swap_plan.md).
Phases 0, 1 and 2 built and on `working`; phase 3 device validation under way
and already the most productive part of the project -- a 3 GB iPhone SE reached
states the 16 GB iPad never does, and found that AOK's growth guard did not
cover the page-fault path at all, so a guest could commit 704 MiB past the point
where the same total in separate `mmap`s was refused. Fixed by sensing in
`tlb_handle_miss` and acting in `handle_timer_interrupt`. Ships **off by
default**, enabled in Settings with a user-specified size. The plan's own
"still open" lists are themselves stale: `/proc/swaps`, `swapon`/`swapoff` and
`mincore` have all landed since they were written.

**Memory truth.** Landed with the above and worth naming separately, because
the swap plan's section 11 deferred it and that deferral is what the plan is
still written around. `mem_resident_page_count` (emu/memory.c:879) is real, and
`/proc/meminfo` now describes the guest rather than the phone
(fs/proc/root.c:479). Before that a guest on a 3 GB phone read `MemTotal` 2957
MB against a real ceiling of 2098 MB, so `free`, `top` and ktop all showed the
machine as ~100% full at idle and no guest allocator could act on any of it.

**The amd64 JIT.** A full amd64 regression suite now runs with **zero**
interpreter fallbacks, closed opcode group by opcode group with
`/proc/ish/amd64_jit` reporting what could not be compiled. The interpreter
entry points the JIT stopped using are deleted. What remains is the dispatch
loop itself, which is a separate question from the fallbacks.

---

## Reported issues

Checked against GitHub on 2026-09-08: **17 open**, down one because
[#503](https://github.com/emkey1/ish-AOK/issues/503) closed with the breakpoint
`si_code` fix. **Closing the issue is part of fixing the bug** -- a fix recorded
here and not there is a fix the reporter never learns about, and #541 is still
proving that point.

### Bugs

| # | Title | Notes |
|---|---|---|
| [#482](https://github.com/emkey1/ish-AOK/issues/482) | Wayland applet does not resize properly | body is a screenshot only. Very likely the same root cause as #483's second half -- confirm before treating them as two jobs |
| [#485](https://github.com/emkey1/ish-AOK/issues/485) | Qt apps (Falkon) cannot connect to session bus | 6 comments |
| [#521](https://github.com/emkey1/ish-AOK/issues/521) | Buildroot `make` crashes on "checking for working sigaltstack" | body is a screenshot only |
| [#523](https://github.com/emkey1/ish-AOK/issues/523) | yay (AUR helper) fails on Arch ARM64 | **reported symptom does not reproduce** -- see *Diagnosed* above. What does reproduce is a TLS handshake tail of 15.3 s against a sub-second median, which is a wait not being woken rather than slow work |
| [#527](https://github.com/emkey1/ish-AOK/issues/527) | pikaur fails on Arch ARM64 | blocked on `systemd-run` |
| [#541](https://github.com/emkey1/ish-AOK/issues/541) | ptraceomatic does not run: tracee reaped during setup | **fixed 2026-08-20**, and still open on GitHub as of 2026-09-08. Close it -- see `docs/historical/build_555_musts.md` §8, and re-run ptraceomatic alongside the ptrace work there rather than closing it blind |
| [#568](https://github.com/emkey1/ish-AOK/issues/568) | Network throughput is very slow for downloads and browsing | split out of #559. Two readings with different causes -- guest-side throughput vs device-wide degradation -- and which one it is has not been settled. Same neighbourhood as #523's handshake tail |
| [#572](https://github.com/emkey1/ish-AOK/issues/572) | Cannot determine a usable wildcard IP (Gradle) | Devuan aarch64 on an iPhone 7 Plus, iOS 15. Gradle wants a bindable local address; worth checking what AOK reports for the interface list before assuming it is a name-resolution problem |
| [#612](https://github.com/emkey1/ish-AOK/issues/612) | Terminal's last row hidden under the keyboard toolbar after returning to the foreground | from Discord (jin2a_), 556 on iPhone18,3: the extra-keys row covers the terminal's bottom line (tmux status) until the terminal is tapped; worse in Arch. Suspect the relayout waits for first responder rather than the foreground -- see #603 (7515ae5c) |
| [#579](https://github.com/emkey1/ish-AOK/issues/579) | Terminal loses keyboard focus after selecting with a Magic Keyboard trackpad | iPad Air M3, iPadOS 26.6.1. Hurts the desktop use case disproportionately: the pointer and the keyboard are the two things a windowed session depends on |
| [#580](https://github.com/emkey1/ish-AOK/issues/580) | Window controls incorrectly positioned in windowed mode on iPadOS | correct in fullscreen, wrong in a window, so it is a layout-guide bug rather than anything deep |
| [#581](https://github.com/emkey1/ish-AOK/issues/581) | makepkg hangs at "Generating .PKGINFO file..." | reporter's own repro steps are incomplete and say so. Unconfirmed. The step it hangs at is `bsdtar`/`fakeroot` work, which is a different neighbourhood from #523 despite both being AUR |

### Feature requests

| # | Title | Notes |
|---|---|---|
| [#483](https://github.com/emkey1/ish-AOK/issues/483) | Wayland applet: derive desktop resolution from window size | **half shipped.** Standalone fullscreen is in `DisplayViewController`; deriving resolution from the window is not -- the client still asks for a fixed pair. The mechanism already exists: `d60437caf` drives per-orientation resize through RFB `SetDesktopSize` |
| [#484](https://github.com/emkey1/ish-AOK/issues/484) | 3D acceleration via virglrenderer | the largest request on the list. Needs a host GL/GLES implementation that iOS does not have, so it is ANGLE-over-Metal or nothing. Wants a feasibility gate of its own before any estimate |
| [#540](https://github.com/emkey1/ish-AOK/issues/540) | External display support (AirPlay) | see *Deferred on purpose* above -- fenced deliberately, and the branch must not be swept into a release |
| [#556](https://github.com/emkey1/ish-AOK/issues/556) | Updated preset appearances | |
| [#559](https://github.com/emkey1/ish-AOK/issues/559) | Feedback: own icons rather than iSH's, more OS images, QEMU | #568 was split out of this thread; the rest is still one issue carrying several asks |
| [#574](https://github.com/emkey1/ish-AOK/issues/574) | Allow for desktop environments | the stack this asks for already installs -- `opt/AOK/tools/setup-wayland.sh` -- so the gap is packaging and documentation rather than capability |
| [#577](https://github.com/emkey1/ish-AOK/issues/577) | Home Screen start-up options | asks for long-press Home Screen quick actions selecting a startup mode. The Shortcuts/AppIntents work shipped in 550 is the machinery this would build on |

---

## Build and test infrastructure

### Linux CI

**Green again as of 2026-08-19**, both arms of the `[clang, gcc]` matrix.

It had been red since 2026-08-10, which is BEFORE the 548 release -- `e4fe5116`,
the commit tagged 548, was itself red. Never a regression of the 549 cycle, and
it affected no shipped code: `build-mac` and `Build Dev IPA` were green
throughout.

Nearly all of it was one root cause: bash's, zsh's and OpenSSH's `config.h` are
each generated by running configure **on a Mac**, and the tree is compiled for
both platforms, so all three asserted Darwin facts that are false on glibc.
iconv lives inside libc on Linux; `<sys/sysctl.h>` and `<sys/filio.h>` are not
glibc headers; `st_atimespec`, `d_namlen` and `fpurge` are Darwin spellings;
`strtonum`, `timingsafe_bcmp`, `memset_s`, `<util.h>`, the `pw_class` family and
`sin_len` are BSD's. Every such macro is now behind `!__linux__`, so the shipping
build is bit-for-bit unmoved. The rest:

- `__thread` must FOLLOW the storage class for gcc -- 40 declarations, mostly in
  the vendored OpenSSH;
- `-D_GNU_SOURCE` project-wide, for `off64_t`, the `cookie_io` typedefs and
  `RUN_LVL`;
- the xattr port, which was a real port and not a config guard: Darwin's calls
  carry a position and an options word and Linux's do not, so the shim now
  declares the shape each platform actually has;
- `<rpc/types.h>`, which OpenSSH asks for and Debian hides in libtirpc -- one
  missing header accounted for 181 of the original 186 failures;
- the fused i386 ALU gadgets, which exist only in aarch64 assembly, so merely
  naming them was a link error on any x86_64 host;
- a duplicate `smallclueRunRsync`, which ld64 quietly tolerates and GNU ld does
  not.

One of the fixes was not a build fix at all. GCC rejected an assignment clang
waves through and turned up a live crash on iOS: `bash --rcfile FILE` and
`--init-file FILE` wrote through a NULL pointer, and native bash runs in-process,
so that is the app going down rather than a shell. See `deps/bash` `a097512`.

Verified by cloning the pushed branch fresh on Debian 13 and building it exactly
the way CI does, with each compiler: 0 failed targets, `float80` and
`riscv64_decode` pass, `e2e` passes.

### `time_conformance` fails only in a full-suite run

Seen 2026-08-20 in a full tier0 sweep: x86_64 reported `time_conformance: FAIL
failures=3`, and the same test passed three times out of three when run alone
immediately afterwards. It is a timing test, the full sweep loads the machine,
and nothing in that run touched clocks -- the only kernel change was
pidfd_open. Recorded rather than diagnosed: if it starts failing alone, it is a
real regression and this note is the date it was not one.

The conductor keeps no per-test log, so the three failing assertions were not
recoverable after the fact. Worth fixing if this recurs -- a failing test that
cannot say what it checked costs a re-run every time.

### Regression-suite observations

From the 4-arch on-device run, 2026-08-19 (aarch64 booted 118/118 clean; i386
110/6, x86_64 112/5, riscv64 103/5):

- **Five failures are identical on every chroot arm and absent from the booted
  arm**: `devtmpfs_mount`, `proc_pid_io`, `taskstats_genl`, `mount_stdev`,
  `fifo_open_creat_deadlock`.
- `mount_stdev` and `devtmpfs_mount` are proven chroot artifacts: they fail in
  an **aarch64** chroot, the same arch that passes them when booted. AOK has no
  mount namespaces, so `/proc` inside a chroot describes the booted root while
  `stat()` sees the chroot's.
- The other three pass when run **by hand** inside the same chroot, and failed
  when x86_64 ran **alone**, so it is the chroot plus the suite runner -- not
  contention and not architecture. Worth understanding before anyone reads them
  as product bugs.
- `mount-root.sh` bind-mounts `/AOK/tools` but not `/AOK/tests`, so
  `setup-regressions.sh` cannot find its sources inside a chroot without a
  manual bind. Small gap worth closing if this becomes routine.
