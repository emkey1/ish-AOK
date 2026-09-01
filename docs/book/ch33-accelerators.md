# 33. The optional accelerators

Chapter 8 removed data movement from the emulator's bill by recognizing libc
functions. Part V removed interpretation by compiling shells and tools in as
host code. Between them they cover a great deal — and they leave a specific gap.

Consider `scp` copying a file. It is not bound by `memcpy`, so high-level
emulation does nothing for it. It is not bound by its own logic, so making it a
native program would buy little. It is bound by **ChaCha20**, which lives inside
libcrypto: not libc, not the program, and hot.

Wayland compositing is the same shape, with pixman in place of libcrypto.

Accelerators are the third mechanism, for work that is neither of the first two.

## 33.1 The rule all three share

**Off by default, opt-in, and a pure fast path.**

Every accelerator here can decline, and declining is not failing. There is
always a path through the ordinary emulator, and the guest gets the right answer
either way — slower.

That property is what makes them safe to have. An accelerator that is mandatory
is a *correctness dependency*: every bug in it is a bug in the system, and it
must be right on every input before anything can ship. An accelerator that is
optional is an optimization, and a wrong answer is a bug in a feature nobody had
to enable.

Chapter 8's HLE follows the same rule from the other side — an unrecognized libc
never matches a fingerprint, and plain translation is always the fallback.

## 33.2 The crypto accelerator

**What it is.** A private syscall — `ISH_SYS_AEAD`, number `0xacc0` — taking a
pointer to a request struct. The guest submits buffers; the host runs
ChaCha20-Poly1305 or AES-256-GCM natively over them, instead of the guest
emulating its own libcrypto instruction by instruction.

`kernel/ish_accel_crypto.c` is a plain, auditable RFC 8439 implementation,
one-shot *and* streaming, with the streaming state carrying across arbitrary
spans. AES-256-GCM is a separate capability with its own self-test.

**Why not fingerprint libcrypto instead.** This is the interesting design
question, because Chapter 8 already built a mechanism for recognizing library
functions and running them natively. It was investigated and rejected, for three
reasons: libcrypto's internal symbols are stripped, Poly1305 is **stateful**
where every HLE function is a pure transform over caller-owned buffers, and
crypto libraries are on the "fastest-rotting update treadmill" — a fingerprint
table would need regenerating with every security release.

That comparison is worth keeping, because it says exactly when HLE is the right
tool: **stable, self-contained, stateless functions with a fixed prologue**. A
cipher is none of those, so a different mechanism was needed.

**How it proves itself.** A cryptographic accelerator that is subtly wrong is a
catastrophe, not a performance regression — so the gate is unusually strict:

- Validated against the RFC 8439 test vectors.
- **3200 and 3000-case differential fuzz against OpenSSL** — one-shot and
  streaming, with random span splits — with zero mismatches.
- And at runtime, `accel_ready()` runs the self-test lazily under a
  `pthread_once` and disables the accelerator if it fails, printing why. Neither
  the CLI nor the app has to remember an initialization call, and a build where
  the implementation is broken silently declines to use it.

Proving a fast path correct against a reference *at startup, on the device it is
running on* is not common, and it is the right level of paranoia for this
particular fast path.

**Zero copy.** `user_transform_two()` walks both guest buffers in lockstep
page-spans and hands the streaming cipher direct host pointers, resolving the
write pointer before the read one per span so it is safe against the memory
layer's copy-on-write and stack-growth lock upgrades (Chapter 5). On
authentication failure the output is scrubbed before returning, because a
partially decrypted buffer is exactly what an attacker would like you to keep.

**And a dedicated operation for one program.** OpenSSH's default cipher uses
`EVP_chacha20` directly rather than through the AEAD interface, so the
accelerator grew a raw ChaCha20 stream operation — 32-bit little-endian counter,
96-bit nonce — to serve it. Chapter 25 gave the consequence: `sshd` stays
emulated and the part of it that matters does not.

## 33.3 The part people miss

An accelerator implemented entirely inside the emulator would be transparent.
This one is not, and the in-app documentation leads with why:

> Nothing uses it automatically. Two pieces have to be in place: **the toggle**,
> in Settings … and **the OpenSSL provider**, a small shared library installed
> *inside your root filesystem*, which is what actually routes OpenSSL's ciphers
> to the syscall.
>
> The second one is the part people miss. The toggle on its own accelerates
> nothing, because nothing in the guest is asking.

That is the structural consequence of the paravirtual design: **the guest has to
consent.** `install-crypto-accel.sh` builds the provider inside the root,
installs it where OpenSSL looks for modules, wires it into `openssl.cnf`, and
then — the part worth copying — *reports which algorithms it took over and how
they time with and without it*. An installer that ends by measuring its own
effect turns "did that work?" into a printed answer.

It also has `--dry-run`, which shows the exact diff it would apply and changes
nothing, and `--uninstall`, which reverses everything. A tool that edits a
configuration file inside somebody's root filesystem should have both.

**The numbers**, and one warning that matters more than they do:

- ChaCha20 on an A10X iPad: about **8.8 MB/s to about 19 MB/s**.
- Real transfers: **1.15x to 1.86x**, depending on device and cipher.
- And: *"Check you are on an optimized build first. At `-O0` the host cipher
  runs 7-17x slower and the accelerator **loses** to plain emulation."*

That last line is Chapter 1's `unoptimized` in `uname -v` earning its keep. An
accelerator that is slower than what it replaces, on a build somebody is using
for development, is exactly the kind of result that gets a good feature
abandoned.

## 33.4 The pixman accelerator

`ISH_SYS_PIXOP` (`0xacc1`) offloads FILL, COPY and OVER on 32-bit
`a8r8g8b8`/`x8r8g8b8` surfaces — the operations that dominate 2D compositing.

It matters because of where pixman sits: cairo uses it for GTK rasterization,
and wlroots' pixman renderer is what a compositor like labwc uses for its own
compositing. So one accelerator speeds up both halves of a Wayland desktop
(Chapter 42), which is measured rather than assumed — **about 23.5% of the
interactive redraw window's wall time was inside raw pixman calls**, consistently
across repeated runs, with a headless labwc and a GTK3 redraw benchmark.

The guest side is an `LD_PRELOAD` shim, and it carries a detail worth having:

> Interposes pixman's public API … plus the property setters needed to know an
> image's state — **pixman has no getter** for transform/repeat/filter/alpha-map/clip.

Interposition can require *shadowing* state the library will not tell you. The
shim cannot ask an image whether it has a transform, so it watches the setters
go past and remembers. That is a general hazard of the technique and a good
reason to check, before committing to interposition, whether the library exposes
enough to know when your fast path is legal.

And the safety property is the same as everything else in this chapter, stated
in the README:

> If unavailable, every interposed function is a pure pass-through to real
> pixman — so it is always safe to load, on stock iSH, real Linux, or with the
> accelerator off.

A shim that is a no-op when its backend is missing can be installed
unconditionally, which is what makes `start-wayland.sh` able to export
`LD_PRELOAD` whenever it finds the library, without checking anything else.

## 33.5 Two ways this dispatch can fail silently

The accelerator syscalls live above every real syscall range, so they fail the
range check and must be intercepted before it — for **every** guest ABI.
Chapter 11 told what happened when they were not: at the time an unknown syscall
got `SIGSYS` here rather than Linux's `ENOSYS`, so probing for an accelerator
that was wired only for arm64 and riscv64 *killed* the probing process on x86
guests, and every consumer's "probe once, fall back on ENOSYS" story died at the
probe. (Unknown syscall numbers answer `ENOSYS` now, which is the other half of
the same fix.)

Two more hazards in the same dispatch were found while designing a *third*
accelerator, and both are silent:

**A binary ternary is correct for two cases.** `calls.c` dispatches i386 and
amd64 through `syscall_num == 0xacc0 ? aead(...) : pixop(...)`. Extending only
the bypass condition to admit a third number routes it into the pixman handler:
a plausible errno, no crash, no log.

**A discarded return value writes no result.** The `bool` returned by
`handle_asm_generic_native_syscall` is not checked, so a number that is in the
bypass but missing from the switch writes **no result register** on arm64 and
riscv64 — `x0`/`a0` still holds the request pointer, which the guest reads as a
very large positive success.

Both belong to a class worth naming: **dispatch code that is correct for N cases
and silently wrong for N+1**. It is not a bug today. It is a bug the next
addition will introduce, and the design note that found it says to name both in
the commit that adds the third number.

## 33.6 The accelerator that has not been built

`docs/metal_sgemm_milestone1.md` is a design study for GPU offload of a single
matrix multiply, and it is included in this chapter because of *how* it reaches
its answer rather than what the answer is.

It was produced by a multi-agent workflow that checked every claim against the
tree, and it **corrected its own brief in three places**:

**The no-copy property is real, but only for a blessed allocation.** A guest
`mmap(MAP_SHARED|MAP_ANONYMOUS, PROT_READ|PROT_WRITE)` reaches `pt_map_nothing`
— one host mapping for the whole range — and gets `P_SHARED`, which
`pt_copy_on_write` skips, so it stays one contiguous host region across `fork`.
A `malloc`'d matrix, a COW-broken page, a `PROT_NONE` promotion or a
stack-growth fault each hand out individual 4 KB host pages instead. Those cases
must return `EOPNOTSUPP` and the guest must fall back to its own implementation.
"Declining is not failing, and the guest shim needs a CPU path regardless" —
Section 33.1's rule, arrived at independently by the study.

**The dispatch needs three edits, not two** — the two silent hazards of Section
33.5, found by reading the code rather than by trusting the brief.

**And no available machine could produce the number.** The Linux build host has
no Metal. The available Mac is an Intel integrated GPU whose throughput says
nothing about an A-series part — and, more subtly, its host page size is 4096,
"so every host-page alignment path is trivially satisfied there and the 16K-page
failures an iDevice would hit are invisible". The go/no-go measurement has to
come from an actual iPad.

Then the part that makes it good experimental design:

> the plan produces a trustworthy answer to "does GPU offload beat the emulated
> CPU for one sgemm", not to "is GPU offload worth pursuing for small ML
> models". A naive MSL kernel plus a per-call `bytesNoCopy` wrap is deliberately
> un-tuned, so **a loss at milestone 1 is not conclusive while a win is.**

An asymmetric experiment, stated as asymmetric before it is run. That is worth
more than most results.

## 33.7 When to build an accelerator

Four conditions, drawn from the two that exist and the one that has not been
started:

**The work is a hot inner loop in library code.** Not libc — Chapter 8 has that.
Not the program's own logic — Part V has that. Something in between, which for
this system means crypto, compositing, and possibly linear algebra.

**The semantics are well-specified and differentially testable.** RFC vectors, a
reference implementation to fuzz against, a bit-exact expectation. If "correct"
cannot be checked automatically against something that is not your own code, do
not build a second implementation of it.

**The guest can be made to ask.** A provider, an `LD_PRELOAD` shim, a patched
call site. If nothing in the guest will route to it, the accelerator accelerates
nothing — and the toggle will look broken.

**Declining is cheap and safe.** Every input the fast path cannot handle must
fall back, and the fallback must be the ordinary correct implementation.

The cost, in every case, is a second implementation of something somebody else
already implemented correctly. That is why the self-test gate exists, why the
fuzz ran for six thousand cases, and why all three of these are off until asked
for.

---

*Anchors:* [kernel/ish_accel.c](../../kernel/ish_accel.c),
[kernel/ish_accel_crypto.c](../../kernel/ish_accel_crypto.c),
[kernel/ish_accel_aes.c](../../kernel/ish_accel_aes.c),
[kernel/ish_accel_pix.c](../../kernel/ish_accel_pix.c),
[kernel/ish_accel_pix_kernels.c](../../kernel/ish_accel_pix_kernels.c),
[kernel/calls.c](../../kernel/calls.c) (the `0xacc0`/`0xacc1` interception),
[opt/AOK/tools/crypto/](../../opt/AOK/tools/crypto),
[opt/AOK/tools/pixman/](../../opt/AOK/tools/pixman),
[opt/AOK/docs/crypto-accel.md](../../opt/AOK/docs/crypto-accel.md),
[docs/performance-optimizations-2026-07.md](../../docs/performance-optimizations-2026-07.md),
[docs/metal_sgemm_milestone1.md](../../docs/metal_sgemm_milestone1.md),
`pixman_accel_plan.md`.

*Story:* an accelerator that loses. At `-O0` the host cipher runs 7–17x slower
than it should, and the crypto accelerator is beaten by the plain emulation it
was written to replace — which is why the build says `unoptimized` in
`uname -v`.
