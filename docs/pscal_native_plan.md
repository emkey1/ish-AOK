# PSCAL in the native image: two routes, and what each costs

Asked 2026-11-27 ("how hard would it be to support the full PSCAL suite in
the native image?"), and shelved the same day ("Not now"). Nothing here is
built. It is an estimate from measuring the code, written down so the next
person starts from the measurements rather than from scratch.

Measured against PSCAL `2f2b83e9` (~/git/pscal, V.1R34 notes), PSCAL's
desktop build of 2026-10-21 in `~/git/pscal/build/bin`, and the AOK Debug
device build at `c1f990561`.

## The short version

| Route | What it is | Work | Speed |
|---|---|---|---|
| **1. Emulated** | Ship PSCAL's static aarch64 Linux binaries in or beside the native image and run them on the arm64 gadget engine, as sshd already runs there | **2-3 days** | 2.4-3.3x slower than native (Stage 0, M5, before HLE went on by default) |
| **2. Native** | Link the frontends into the app as `/AOK/native/<name>` programs, like zsh, dash and bash | **4-6 weeks** for pascal, clike, rea, aether, pscalvm and pscaljson2bc (phases 0-6 below add up to 19-30 working days); **exsh +1.5-2.5 weeks** | native |

Recommendation when this is picked up: **route 1 first**, then time real
programs on the A10X. Go native only if the speed matters to someone, the
frontends first and exsh last.

"The full suite" here means what the PSCAL rootfs ships: the five frontends
(`pascal`, `clike` with `clike-repl`, `rea`, `aether`, `exsh`), the VM tools
(`pscalvm`, `pscaljson2bc`), optionally `pscald`/`pscalasm` (`BUILD_PSCALD`)
and `dascal` (`BUILD_DASCAL`). SmallCLUE and the OpenSSH client are already
native in AOK. **Graphics (SDL, PSCAL's GL shaders and terrain code) are out
of scope on both routes.**

## Route 1: emulated binaries

**Taken, 2026-11-27, as packages.** PSCAL is the `pscal` .deb in
AOK-Workstation's own apt repository, [emkey1/aok-apt](https://github.com/emkey1/aok-apt)
(served at https://emkey1.github.io/aok-apt), built for Devuan 6 on arm64 and
amd64 by that repository's workflow. A native root's companion has the source
(`/AOK/tools/aok-apt.sh`, run by `setup-companion.sh` and once by the apt
stub), so `sudo apt install pscal` puts it in the companion, where it runs
emulated and is found by name from the native root
(opt/AOK/docs/native-companion.md). These are ordinary dynamically linked
Debian builds, not the static rootfs binaries described below, and the
install root is `/usr/lib/pscal`. On the M4 iPad (2026-11-27), installed into
the native root's companion and run by name from the native root, a small
program in each language took 44-110 ms end to end, emulated. The notes below
are what was weighed before that.

A native root runs guest ELFs too: native dispatch only takes over for
`/AOK/native/*`, and everything else goes through the gadget engine. sshd in
a native root is already a static aarch64 ELF run that way.

- **Binaries.** The PSCAL rootfs series (`pscal-rootfs-<date>-aarch64.tar.xz`;
  the 2026-09-19 build is 8.7 MB with HTTP) already holds static glibc
  builds of all of them. They are built by `~/git/pscal/tools/build_pscal_rootfs_docker.sh`
  in an arm64 `debian:bookworm-slim` container (about 3 hours; bookworm, not
  trixie, because of SmallCLUE's vendored OpenSSH). Static glibc resolves DNS
  through c-ares, which that build links for this reason.
- **Where they go**, a decision to make:
  - `/AOK/bundled`, served from the app bundle: every user pays ~9 MB of app
    size, nothing to download, always matches the app.
  - A download into the native root, like a catalogue add-on: only the people
    who want it pay.
  - Not the companion root: PSCAL is not a Devuan package.
- **Runtime files.** The frontends find their units, headers and modules
  under an install root fixed at build time (`PSCAL_INSTALL_ROOT`; `lib/`
  holds `pascal`, `clike`, `rea`, `misc`, `sounds`, 632 KB) and overridable
  with `PSCALI_INSTALL_ROOT` (and `PSCALI_ETC_ROOT`). Point that at wherever
  the files land, from `/etc/profile.d` or a wrapper.
- **Work:** packaging and the install path, PATH order against
  `/AOK/native` and `native-links.sh` (the image must not bring its own
  SmallCLUE in front of the app's), then PSCAL's own test suite on a device.
- **Known issue to check first:** Stage 0 found `exsh`'s
  `thread_worker_reuse` failing 10 of 10 in the guest: `joinThreadInternal()`
  (pscal-core `src/vm/vm.c`) returns without waiting when a new worker has not
  yet set `active`, and `vmJoinThreadById()` ignores that. A PSCAL bug that a
  fast host hides. Check whether it is fixed upstream.
- **Speed baseline to re-take:** Stage 0's numbers (hello world 46 ms vs 18 ms
  native, `aether --no-cache` 66 vs 20 ms) predate HLE being on by default
  (2026-09-30), and HLE then made PSCAL slower. Time again on the A10X before
  deciding anything on speed.

## Route 2: native programs

### What is already in our favour

- **Every frontend already has an in-process entry point.** PSCAL's iOS app
  cannot spawn, so exsh's `shellSpawnToolRunner`
  (`components/exsh/src/shell/runner.c`) calls `pascal_main`, `clike_main`,
  `rea_main`, `aether_main`, `pscalvm_main` and `pscaljson2bc_main` directly,
  repeatedly, in one process, and `fflush(NULL)`s after each. That is exactly
  the shape of AOK's registry (`kernel/native.c`: name -> `main`).
- **Running two at once is designed for.** `PSCAL_THREAD_LOCAL`
  (`components/pscal-core/src/core/globals.h`) marks per-run state in 347
  places; on iOS it is `_Thread_local`, because a pipeline's stages and a
  terminal's tabs are threads of one process. It is wrapped in
  `#ifndef PSCAL_THREAD_LOCAL`, so **`-DPSCAL_THREAD_LOCAL=_Thread_local`
  turns it on without `PSCAL_TARGET_IOS`** and its vproc layer. The same
  header says a VM worker gets its own copy of that state (`ThreadJob` in
  `vm.c`).
- **There is a gate for the rest.** `Tests/ios_host_statics.sh <build dir>`
  lists the mutable statics left in an iOS-flavoured build, against an
  allowlist of state that is process-wide on purpose. It exists because a
  shared static in the compile path crashed `clike a | clike b`.
- **`exit()` is mostly handled.** `SUPPRESS_EXIT` (also `#ifndef`-guarded,
  so it can be defined alone) turns `EXIT_FAILURE_HANDLER()`, used 154 times,
  into a message. Six raw `exit(` calls remain, plus whatever third-party code
  does.
- **Plugins can be switched off.** `dlopen` appears only in vproc (iOS) and
  `ext_builtins/plugin_loader.c`, which `PSCAL_EXT_PLUGINS_DISABLED` removes.
  iOS would refuse to load an unsigned plugin anyway.
- **Licence:** Apache-2.0, so none of the bash problem.
- **Symbols do not collide.** The app's Debug build exports 124,590 symbols;
  pscal-core plus the pascal frontend's static libraries export 2,605, and
  1,311 of them clash. Taking out the symbols of AOK's own SmallCLUE and
  OpenSSH archives leaves 189, and those are nextvi (another shared vendored
  copy), `__ft`/`__ph`-style editor tables from it, `_number`, and two
  functions AOK stubs for SmallCLUE (`pscalRuntimeStderrIsInteractive`,
  `pscal_openrsync_main`, in `kernel/smallclue_glue.c`). PSCAL's own code
  clashes with nothing in bash, zsh or dash.

### What it costs

The numbers are working days for one person who knows the native-program
rules (docs/native_mode_plan.md, docs/bash_native_plan.md).

**Phase 0, a spike that answers the unknowns (2-3 days).** Build pscal-core
and `pascal` for the CLI (meson) with the shim force-included and
`-DPSCAL_THREAD_LOCAL=_Thread_local -DSUPPRESS_EXIT -DPSCAL_EXT_PLUGINS_DISABLED`,
no vproc, no path virtualisation; register it as `/AOK/native/pascal`; run
hello world and a threaded program in a guest. Then run
`tools/check-native-libc.py` on its objects and `ios_host_statics.sh` on the
build. That gives the real libc list, the real leftover statics, and whether
VM workers see what they need. **Re-estimate everything below from what it
finds.**

**Phase 1, build integration (3-5 days).**
- Today's `libpscal_core_static.a` is 15 MB because it bundles SmallCLUE,
  OpenSSH and nextvi. AOK already links its own copies, so the core has to be
  built without them, against AOK's (and AOK's SmallCLUE pin may differ from
  PSCAL's; the frontends barely use it, exsh does).
- pscal-core is 188k lines (backend_ast 70k, runtime 28k, third_party 17k
  (yyjson), vm 17k, compiler 17k, core 14k, ext_builtins 11k); the frontends
  add pascal 8k, clike 6k, rea 12k, aether 31k. Bring the source in the way
  `deps/bash`, `deps/zsh` and `deps/smallclue` come in (a submodule of a fork
  the project controls), and build it in **both** the Xcode project (the build
  that counts) and meson (for CLI tests).
- Replace the two SmallCLUE stubs with the real functions.

**Phase 2, routing libc through the shim (1-1.5 weeks).** The desktop
`pascal` binary has 854 undefined symbols. The ones that need a decision:
- process creation: `fork`, `execv`/`execve`/`execvp`, the `posix_spawn`
  family, `system`, `popen`, `waitpid`, `kill`. These are PSCAL's shell-out
  builtins; route them to the shim's spawn path, as SmallCLUE's are.
- the terminal: `tcgetattr`, `tcsetattr`, `ioctl`, `isatty` (REPLs, the CRT
  unit).
- signals and threads: `signal`, `sigaction`, `pthread_create`,
  `pthread_join`. Signal delivery to a native program's own thread is the
  shim's job already; check Ctrl-C reaches a running VM.
- the filesystem: `opendir`/`readdir`, `realpath`, `mmap`, and everything
  stdio. Anything that reaches the host libc reads the iOS filesystem
  instead of the guest's, silently.
- accounts and environment: `getpwnam`, `getpwuid(_r)`, `getenv`, `setenv`.
- the network: `socket`, `connect`, `socketpair`, `select`, `getaddrinfo`,
  and libcurl. See phase 5.

**Phase 3, state between and during runs (2-4 days, if phase 0 confirms the
VM worker copy).** Each frontend has about 1,130 writable globals on the
desktop build (pascal 1,127, clike 1,141, rea 1,158, aether 1,211, pscalvm
1,121), against 90 thread-local; the iOS flavour moves the 347 marked ones to
thread-local. Clear `ios_host_statics.sh`'s list; check that a frontend run
leaves nothing behind for the next (a native program is a function call, so
every early return and suppressed exit leaks unless cleaned up); run two
different frontends at once and the same one twice in a pipeline.

**Phase 4, runtime files (1-2 days).** Serve the 632 KB `lib/` tree
read-only from `/AOK` (as `/AOK/tools` is), set the install root to it, and
let the bytecode cache write to the guest's home through the shim.

**Phase 5, HTTP (1-3 days).** All five frontends have had HTTP since
2026-09-19 through libcurl. Do not bring libcurl in: AOK already has a libcurl
stand-in on NSURLSession, `deps/smallclue-shim/curl_nsurlsession.m` (built as
`curlshim`, used by SmallCLUE's curl and wget), so TLS, certificates and DNS
are the system's. It implements every curl function the desktop `pascal`
calls (`curl_easy_init`, `_setopt`, `_perform`, `_getinfo`, `_cleanup`,
`_strerror`, `curl_slist_append`, `_free_all`) except **`curl_easy_reset`**,
which would need adding. Then check the `CURLOPT_*` and `CURLINFO_*` values
PSCAL's network builtins (`backend_ast/builtin_network_api.c`) pass against
the ones the stand-in handles, and read the header's note on what host
networking gives up.

**Phase 6, tests (about a week).**
- PSCAL's own suite against the native binaries, in the CLI and on a
  device. Clean the `/usr/local/bin` symlink farm first: a flagless native
  `ls` there once took that suite from 217/0 to 116/1.
- Two frontends at once, in two terminals and in one pipeline.
- Ctrl-C and Ctrl-Z in a running program and in a REPL.
- Checkpoint and suspend: either refuse a checkpoint while a native frontend
  runs (as dash does) or define what a restore does.
- The licence notices and the About screen's acknowledgements.

**Phase 7, exsh (1.5-2.5 weeks, separate).** The 2026-11-12 evaluation in
docs/TODO.md still holds: exsh's pipeline stages are threads that must see
the shell's own state (`gShellRuntime.pipeline`, the VM), so thread-local
globals break it; it needs a per-shell struct behind a TLS pointer that the
shim's `nlibc_pthread_create` hands to each child thread. Until then exsh can
run emulated (route 1) and still start the native frontends, since native
dispatch serves an emulated caller.

**App size:** about 8-12 MB more for the core and the frontends without their
bundled SmallCLUE and OpenSSH (the desktop binaries are ~8.5 MB each because
each carries all of that, plus libcurl and OpenSSL).

## Decisions to make before starting

1. Which tools count: the seven above, or also `pscald`, `pscalasm` and
   `dascal`.
2. Route 1's install location: in the app, or downloaded.
3. HTTP in the first native version (through the existing NSURLSession
   stand-in), or later.
4. Where the PSCAL-side changes land (build options, the smallclue split):
   they are the maintainer's own repository either way.
5. How PSCAL is brought in: a fork submodule pinned to a PSCAL commit, or
   prebuilt archives from PSCAL's build.

## How the numbers were taken

- Line counts: `.c`, `.h` and `.inc` files, tests excluded, per component.
- Writable globals: `nm -m` entries in `__DATA,__data/__bss/__common` on
  each desktop binary; thread-local: entries in `__thread_vars/bss/data`.
- Undefined symbols: `nm -u` on the desktop `pascal`.
- Collisions: `nm -gU` of the app's `iSH-AOK.debug.dylib` against
  `nm -gU` of `libpscal_core_static.a` and `libpscal_pascal_static.a`, then
  minus `nm -gU` of `build/libsmallclue*.a` and `build/libopenssh*.a`.
- The other facts are read from the files named beside them.
