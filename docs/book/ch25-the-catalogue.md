# 25. The catalogue

Nine entries live in the registry, and everything else under `/AOK/native` is a
symlink to one of them:

| entry | what it is |
|---|---|
| `smallclue` | a busybox-style MIT toolbox; applet chosen by `argv[0]` |
| `motepad` | a modeless terminal text editor |
| `hx` | [helix](https://helix-editor.com), a modal editor with syntax highlighting |
| `bash` | GNU bash, behind a build option (Chapter 26) |
| `zsh` | zsh, on by default |
| `zsh-multio` | the byte pump behind zsh's MULTIOS redirections |
| `rust-probe` | exercises the Rust-on-the-shim path |
| `bmm`, `bmt` | the `/AOK/tools` benchmarks, as native programs |

This chapter is about what it took to get each of them in, because the interesting
part is never the program — it is the thing about being a function call that the
program was not written for.

## 25.1 SmallCLUE, and the applets that do not own their arguments

SmallCLUE is a multicall toolbox in the busybox tradition: one binary, dozens of
applets, `argv[0]` selecting which. It is MIT-licensed, it is a submodule
(`deps/smallclue`), and it is the vehicle for most of what follows — OpenSSH and
the Nextvi editor are both applets of it.

Its most instructive bug is about ownership.

> **The bug that taught us this**
>
> A natively-dispatched applet does **not** own its `argv`. That array is the
> kernel's native-exec pending record (Chapter 22): `native_dup_vector` allocates
> `count + 1` slots, and `native_free_vector` frees it by **walking to the first
> NULL** and then freeing the array.
>
> So the common idiom — strip a global option out of `argv` by shifting the rest
> down and decrementing `argc` — is a double free. Shifting leaves the vacated
> tail slots holding stale copies of pointers that now live earlier in the array,
> and the kernel's walk, which knows nothing about the applet's decremented
> `argc`, frees each of those twice.
>
> It presents as `BUG IN LIBMALLOC: malloc assertion "main_address" failed`, and
> `ish` exiting 133 — **after printing the correct answer.**
>
> The fix shape: gather the survivors into a vector of the applet's own and leave
> `argv` untouched. And explicitly *not* the tempting alternative of NULLing out
> the vacated tail slots, because that stops the kernel's free walk early and
> leaks the stripped strings instead.

That is Chapter 22's rule wearing different clothes. An ordinary program owns
everything it was handed, because the kernel that handed it over is gone. A
native program shares its arguments with a caller that is still running and will
clean up afterwards.

## 25.2 OpenSSH: re-enabling, not porting

`ssh`, `scp`, `sftp`, `ssh-keygen` and `ssh-copy-id` are applets of SmallCLUE,
and the story of getting them working is a good corrective to the assumption
that vendoring a large program means porting it.

They were **disabled, not missing**. The build excluded three files, and the glue
stubbed the five commands to print "not built". Underneath, `openssh_app.c` was a
~2,400-line launcher that already solved the hard native-program problems:
`setjmp`/`longjmp` around OpenSSH's `fatal()` and `exit()` — because a native
program must not kill the application (Chapter 22) — a pty layer, and a
run-once/run-again split for re-entry.

The blockers were dependencies rather than design: OpenSSL for the crypto and
zlib for compression. Both turned out to be avoidable — OpenSSH configures
`--without-openssl` and uses its own implementations, which costs the key types
that only OpenSSL provides and leaves ed25519, which is what anybody generating
a key today uses anyway.

The lesson is worth stating because it generalizes to every vendored dependency
in this part: **read what the previous attempt actually left behind before
assuming it left nothing.** A disabled build target with weak symbols waiting to
be overridden is most of a port.

## 25.3 Nextvi, motepad, and a stop flag

`vi` is Nextvi, another SmallCLUE applet, and it is where Chapter 22's rule was
first paid for in full — the `xquit` flag that made the second `vi` of a session
draw one frame and exit successfully.

`motepad` is the fork's own: a modeless terminal text editor, and the terminal
counterpart to the MotePad applet in Workspace (Chapter 31). It exists partly
because "an editor that behaves the way a phone user expects" is not something
the vi family offers, and partly because a program written *knowing* it is a
native program is a useful thing to have in the tree — it is the reference for
how the entry, exit and cleanup are supposed to look.

## 25.4 helix, and the four silent traps of a foreign toolchain

`hx` is helix: a modal editor written in Rust, with tree-sitter grammars for
syntax highlighting linked into the binary.

Rust is where Chapter 23's `#define` mechanism stops working entirely. Cargo
compiles its own objects with its own toolchain, and those objects import the
real libc symbol names. The route is `llvm-objcopy --redefine-sym`, driven by
`tools/gen-nlibc-renames.py` reading the shim header and applied by
`tools/build-rust-native.sh`.

Getting that pipeline right took four attempts, and what makes them worth
recording is that **every one of them failed silently first**:

**`ld -r` on an archive produces a lie.** With `-all_load` or `-force_load` it
emits an 11 MB object holding four symbols, and exits 0. It links. Every call in
it goes nowhere. The correct sequence is `ar x` to extract the members, then a
partial link of those.

**A meson `declare_dependency(link_args: ...)` reaches the CLI build and nothing
else.** Xcode links `-lish` against the archive meson produced; it does not link
*through* meson. Anything a native program needs must be **inside** `libish.a`,
which means `objects:` on the library target. A change that works perfectly in
the terminal and does nothing on the device is the most expensive kind of
correct.

**Darwin exports suffixed symbols that the compiler prefers.** `realpath` emits
`_realpath$DARWIN_EXTSN`; x86_64's stat family emits `$INODE64`. A rename list
keyed on bare names misses all of them — so `fs::canonicalize` resolved against
the *Mac*, with every check passing. The generator now emits `$DARWIN_EXTSN`,
`$INODE64`, `$UNIX2003` and `$NOCANCEL` variants for every name.

**`llvm-objcopy` skips some undefined symbols in Mach-O archive members, and
still exits 0.** Flattening to one object first makes it rewrite all of them —
which by itself removed a documented exception that had been carried as
unavoidable.

Four tools, four zero exit statuses, four wrong results. The pattern in all of
them is that the *toolchain* reported success while the *outcome* was wrong,
which is the same shape as Chapter 18's btop episode at a different layer, and
has the same remedy: check the outcome, not the invariant.

## 25.5 Async Rust, and a kqueue front end

Rust running is one thing. Rust's async ecosystem is another, because tokio is a
reactor: it wants an event notification mechanism, a signal-handling
socketpair, and a set of per-process globals that live for the life of the
process.

Every one of those is a problem here. The event mechanism has to be AOK's, not
the host's, or the reactor waits on host descriptors that mean nothing to the
guest — so there is a **kqueue front end** (`kernel/native_kqueue.c`) that
answers tokio's `kevent` calls out of AOK's own readiness layer (Chapter 14).
The per-process globals are the invocation-token problem of Chapter 22, which is
why the project maintains forks of `tokio` and `signal-hook-registry` keyed on
`nlibc_invocation_token()`.

Real Rust runs, async Rust included. `rust-probe` exists to exercise that path
deliberately — it is not a tool anybody has a use for, and the registry entry
says so.

## 25.6 The benchmarks

`bmm` and `bmt` are the `/AOK/tools` benchmarks compiled in as native programs,
and their presence in the registry is a small piece of methodological hygiene.

A benchmark that runs as an emulated guest binary measures the emulator plus
itself. A benchmark compiled in as a native program measures what it is aimed
at. `bmt` is the thread-storm benchmark that found the ghost-task bug of Chapter
10 by actually reaching the host's thread limit — which an emulated benchmark
would have taken far longer to do, if it managed it at all.

## 25.7 What the catalogue is not

Two absences are worth naming, because they show where the boundary of this
technique sits.

There is no native Python, no native Node, and no native browser engine. The
test from Chapter 23 explains why: a library or runtime can be linked when its
API splits cleanly into a pure half and an I/O half. Where I/O runs throughout —
and in a language runtime it does — the whole thing needs link-time symbol
rewriting, which is exactly what the Rust pipeline does and exactly how much
work that turned out to be.

And there is no native `sshd`. The blocker is specific and instructive: `sshd`
forks for privilege separation, in `sshd-session.c`, and that fork is not one
the re-launch trick of Chapter 24 can serve — it is not a shell transferring
state to a copy of itself, it is a privileged process dropping privilege in a
child while retaining it in the parent. The decision recorded in the tree was to
choose re-launch over patching privilege separation out, and it remains open.

It is also less costly than it sounds, because the emulated cost is not spread
evenly. An `ssh` session is **cipher-bound** — measured, when somebody asked
"would large `scp`s benefit?" — and the cipher is precisely what the crypto
accelerator of Chapter 33 takes out of the emulator. The guest's OpenSSL routes
ChaCha20-Poly1305 and AES-GCM through a private syscall to a host-native
implementation, and OpenSSH's own default cipher was specific enough that the
accelerator grew a dedicated `EVP_chacha20`-compatible raw stream operation to
serve it. Measured on an A10X iPad: about 8.8 MB/s to about 19 MB/s, and
1.15–1.86x on real transfers depending on device and cipher.

So the honest summary is not "sshd is slow because it is not native". It is that
the part of `sshd` worth accelerating has been accelerated by a different
mechanism, and what remains emulated is connection setup and protocol
bookkeeping rather than the bulk work.

## 25.8 The class of bug this part is about

Read the catalogue back and one pattern accounts for most of it.

Every program here was written by people who assumed a process. They assumed
their globals started fresh, their `argv` was theirs, their `exit()` ended
something, their signal handlers belonged to them, their file descriptors were
theirs to close, and their `fork` worked.

None of those assumptions is malformed. They are what a program *is*, and no
reasonable author states them. They simply stop being true when the program
becomes a function call in an application that has been running for hours and
may run several copies of it at once.

That is why Part V has more failure stories per page than any other part of this
book, and why the tooling around it — the allowlist gate, the invocation token,
the TLS conversion checkers, the argv audit — is disproportionate to the amount
of code involved. The code is small. The assumptions are enormous, and they are
invisible until they break.

---

*Anchors:* [kernel/native.c](../../kernel/native.c) (the registry),
`deps/smallclue`, [kernel/smallclue_glue.c](../../kernel/smallclue_glue.c),
[kernel/openssh_glue.c](../../kernel/openssh_glue.c),
[kernel/nextvi_glue.c](../../kernel/nextvi_glue.c),
[kernel/native_motepad.c](../../kernel/native_motepad.c),
[kernel/native_kqueue.c](../../kernel/native_kqueue.c),
[kernel/native_bench.c](../../kernel/native_bench.c),
`deps/helix`, `deps/tokio`, `deps/signal-hook`,
[tools/build-rust-native.sh](../../tools/build-rust-native.sh),
[tools/gen-nlibc-renames.py](../../tools/gen-nlibc-renames.py),
[tools/native-applet-audit.py](../../tools/native-applet-audit.py),
[docs/native_workspace_design.md](../../docs/native_workspace_design.md).

*Story:* `fs::canonicalize` resolving against the Mac — because Darwin exports
`_realpath$DARWIN_EXTSN` and the symbol-rename list was keyed on bare names, so
every check passed and the answer came from the wrong filesystem.
