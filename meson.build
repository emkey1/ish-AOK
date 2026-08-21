project('ish', 'c',
    default_options: ['default_library=static', 'c_std=gnu11', 'warning_level=2'])
cc = meson.get_compiler('c')

if cc.get_id() == 'clang'
    add_project_arguments('-Wimplicit-fallthrough', '-Wtautological-constant-in-range-compare', language: 'c')
endif

if get_option('b_sanitize').split(',').contains('undefined')
    add_project_arguments('-fno-sanitize=alignment', language: 'c')
endif

log_on = get_option('log').split()
log_off = get_option('nolog').split()
foreach channel : log_on + log_off
    if log_on.contains(channel)
        add_project_arguments('-DDEBUG_' + channel + '=1', language: 'c')
    else
        add_project_arguments('-DDEBUG_' + channel + '=0', language: 'c')
    endif
endforeach
# glibc hides a good deal behind feature-test macros that Darwin exposes anyway
# -- off64_t and the cookie_io typedefs, RUN_LVL in <utmpx.h>, and more. Darwin
# ignores the macro, so it is set for everyone rather than gated.
add_project_arguments('-D_GNU_SOURCE', language: 'c')
add_project_arguments('-DLOG_HANDLER_' + get_option('log_handler').to_upper() + '=1', language: 'c')
add_project_arguments('-DENGINE_' + get_option('engine').to_upper() + '=1', language: 'c')

if get_option('no_crlf')
    add_project_arguments('-DNO_CRLF', language: 'c')
endif

add_project_arguments('-Wno-switch', language: 'c')

# Whether the deps/bash submodule is populated, decided HERE rather than beside
# the bash target further down: kernel/native.c's registry needs the answer as a
# define, and add_project_arguments has to run before any target is declared.
#
# A weak symbol would keep this local to one file and was tried first. It does
# not work: ld64 will not leave an unresolved REFERENCE in a static initializer,
# weak_import included, so a build without deps/bash failed to link on the table
# entry itself.
fs_early = import('fs')
bash_opt = get_option('native_bash')
bash_present = fs_early.exists('deps/bash/shell.c')
if bash_opt.disabled()
    # Not merely unregistered: libbash never gets built, so link_whole below
    # folds nothing in. See meson_options.txt for why that distinction matters.
    have_bash = false
elif bash_opt.enabled() and not bash_present
    error('-Dnative_bash=enabled but deps/bash is not checked out. Either:\n' +
          '    git submodule update --init deps/bash\n' +
          '  or build without it: -Dnative_bash=disabled')
else
    have_bash = bash_present
endif
if have_bash
    # Populated but STALE is the case worth catching by name. A pull that moves
    # the submodule pointer does not move the submodule, so deps/bash can be a
    # bash without the sources AOK's build lists -- and the error that produced
    # was `File deps/bash/aok_fork.c does not exist`, which says nothing about
    # what to do. Checked here so the whole configure stops with the command.
    if not fs_early.exists('deps/bash/aok_fork.c')
        error('deps/bash is checked out at an older commit than this build ' +
              'expects (aok_fork.c is missing). Update it with:\n' +
              '    git submodule update --init deps/bash')
    endif
    add_project_arguments('-DISH_NATIVE_BASH=1', language: 'c')
endif

# The same question for zsh, and the same reason it is answered here: the
# registry in kernel/native.c needs it as a define before any target exists.
#
# deps/zsh is a CONFIGURED tree rather than a pristine checkout, and that is
# what the test looks for. zsh generates a great deal of its own source --
# config.h, Src/signames.c, every module's .mdh and .epro, Src/bltinmods.list
# -- and this build compiles those generated files rather than reproducing
# zsh's mkmakemod.sh in meson. So the marker is config.h, not a .c: a tree
# someone unpacked but never configured would otherwise fail much later with a
# missing "../config.h" from inside a generated header.
zsh_opt = get_option('native_zsh')
zsh_present = fs_early.exists('deps/zsh/config.h') and fs_early.exists('deps/zsh/Src/bltinmods.list')
if zsh_opt.disabled()
    have_zsh = false
elif zsh_opt.enabled() and not zsh_present
    error('-Dnative_zsh=enabled but deps/zsh is not a configured zsh tree.\n' +
          '  It is a submodule now, and the generated sources are committed in\n' +
          '  it, so there is nothing to configure -- just check it out:\n' +
          '    git submodule update --init deps/zsh\n' +
          '  To re-generate them instead (a new zsh, or different options):\n' +
          '    cd deps/zsh && ../../tools/configure-zsh.sh && make prep\n' +
          '  Or build without zsh: -Dnative_zsh=disabled')
else
    have_zsh = zsh_present
endif
if have_zsh
    add_project_arguments('-DISH_NATIVE_ZSH=1', language: 'c')
endif

# Said out loud at configure time, because the consequence is a licensing one
# and the alternative is finding out from an App Store takedown. Nothing else
# in the binary is GPL: smallclue is MIT, OpenSSH and libarchive BSD, liblzma
# public domain, and iSH-AOK's own GPL is what LICENSE.IOS covers.
summary({'native bash': have_bash ? 'yes -- binary contains GPLv3 (bash, readline)'
                                  : 'no -- no third-party GPL in the binary'},
        section: 'Licensing')
# zsh is in the OTHER column and says so, because "another shell compiled in"
# reads like the same licensing question and is not: zsh's licence is
# permissive, and the only GPL files in its tree are three completion SCRIPTS
# that nothing here compiles or installs.
summary({'native zsh': have_zsh ? 'yes -- permissive (zsh LICENCE), no GPL compiled'
                                : 'no'},
        section: 'Licensing')

# Resolved here (rather than next to the gadget source lists below) because
# it also drives a project-wide define, and add_project_arguments must run
# before any target is declared. The arm64-GUEST gadgets
# (jit/guest-arm64/*.S) are literal AArch64 assembly, only built when the
# HOST gadget set is aarch64 — ISH_JIT_ARM64_GUEST tells the C side
# (jit/gen.c) whether those gadget symbols exist, so non-aarch64 hosts
# (e.g. the x86_64 CLI test build) compile a clean unsupported stub instead
# of failing to link.
jit_cpu_family = get_option('jit_cpu_family')
if jit_cpu_family == ''
    jit_cpu_family = host_machine.cpu_family()
endif

# Guest architecture selection. Default is every supported guest; each can be
# disabled with e.g. -Dguest_archs=i386,amd64. At least one must remain. The
# ISH_GUEST_* defines gate ELF detection (kernel/exec.c) so a disabled arch's
# binaries fail exec with ENOEXEC. For arm64 (and future guest-gadget arches)
# disabling also drops the engine sources; the i386/amd64 translation units
# stay compiled regardless because the x86 decoder template, the host gadget
# sets, and jit/gen.c are interwoven (gadgets-*/[control,math].S call
# amd64_jit_* helpers from emu/amd64_interp.c) — for those two the option is
# a functional gate, not yet a size optimization.
guest_archs = get_option('guest_archs')
if guest_archs.length() == 0
    error('guest_archs must enable at least one guest architecture (i386, amd64, arm64, riscv64)')
endif
foreach garch : ['i386', 'amd64', 'arm64', 'riscv64']
    add_project_arguments('-DISH_GUEST_' + garch.to_upper() + '=' + (guest_archs.contains(garch) ? '1' : '0'), language: 'c')
endforeach

# Which instruction sequence the arm64-guest gadget dispatcher uses to load the
# next gadget pointer. This exists because the right answer is host-generation
# dependent and the difference is large: for the x86 gadgets, ldar measured 4.3%
# FASTER on Apple Silicon and 2.04x SLOWER on an A9 (ARMv8.0) -- see the warning
# in jit/gadgets-aarch64/gadgets.h and commits 861da1d1 / 95140ab7. The arm64
# guest still dispatches through ldar, so it may be paying that same penalty on
# ARMv8.0 hardware; this switch is how that gets A/B'd on a device without
# hand-patching a header. Default 'ldar' keeps existing behaviour.
add_project_arguments('-DISH_ARM64_GRET_' + get_option('arm64_gret').to_upper() + '=1', language: 'c')

if jit_cpu_family == 'aarch64' and guest_archs.contains('arm64')
    add_project_arguments('-DISH_JIT_ARM64_GUEST=1', language: 'c')
elif guest_archs.contains('arm64')
    warning('arm64 guest enabled but host gadget set is ' + jit_cpu_family
            + ': the arm64 engine is aarch64-host-only, arm64 binaries will not run')
endif

if jit_cpu_family == 'aarch64' and guest_archs.contains('riscv64')
    add_project_arguments('-DISH_JIT_RISCV64_GUEST=1', language: 'c')
elif guest_archs.contains('riscv64')
    warning('riscv64 guest enabled but host gadget set is ' + jit_cpu_family
            + ': the riscv64 engine is aarch64-host-only, riscv64 binaries will not run')
endif

includes = [include_directories('.')]

threads = dependency('threads')
librt = cc.find_library('rt', required: false)
libm = cc.find_library('m', required: false)
libdl = cc.find_library('dl', required: false)
# gcc/glibc on x86_64 lower the aarch64-guest CASP 128-bit compare-and-swap
# (emu/tlb.c's arm64_casp) to a __atomic_compare_exchange_16 libcall rather
# than inlining it, which needs -latomic. Apple clang always inlines it, so
# this is a no-op there.
libatomic = cc.find_library('atomic', required: false)
sqlite3 = cc.find_library('sqlite3')
# zlib, for SmallCLUE's tar and gzip. Public on macOS AND iOS -- both SDKs ship
# zlib.h and libz.tbd -- so unlike a vendored library this costs nothing but the
# link. required: false so a host without it still builds, with tar and gzip
# excluded exactly as they were before.
zlib = cc.find_library('z', required: false, has_headers: ['zlib.h'])
if zlib.found()
    # kernel/smallclue_glue.c defines tar/gzip/gunzip/zcat stubs when this is
    # absent. add_project_arguments has to run before any target is declared,
    # which is why it is here rather than beside the applet list below.
    add_project_arguments('-DISH_HAVE_ZLIB=1', language: 'c')
endif

# curl, wget and `md <url>` need libcurl, which the iOS SDK does not have in
# any form -- no header, no tbd. deps/smallclue-shim/curl/curl.h supplies the
# slice of the easy API SmallCLUE uses on top of NSURLSession, which needs an
# Objective-C compiler. add_languages must run before any target is declared.
#
# required: false so that a toolchain without one still builds, with curl and
# wget refusing honestly as they do today. Apple-only for the obvious reason:
# elsewhere there is no Foundation to implement it with.
# A Rust program compiled into the binary, reached the only way a foreign
# toolchain can be: by rewriting its libc imports onto the shim.
#
# kernel/native_libc.h redirects libc by #define, which reaches only the
# translation units AOK compiles. Rust's std is not one of them, so its
# open()/read() bind to the host at link time and would resolve against iOS.
# tools/gen-nlibc-renames.py reads the header and emits the objcopy flags that
# rewrite exactly the set it redirects -- generated rather than listed so the
# two cannot drift.
#
# Not the linker's -alias, which was the first thing that worked and is wrong:
# an alias is global, so it would redirect AOK's OWN calls too, and the kernel
# must reach the real host open(). Rewriting one archive is the scoped version.
#
# required: false throughout -- a machine without a Rust toolchain builds
# everything else exactly as before.
cargo = find_program('cargo', join_paths(get_option('cargo_home'), 'bin', 'cargo'), required: false)
llvm_objcopy = find_program('llvm-objcopy', '/opt/homebrew/opt/llvm/bin/llvm-objcopy',
                            '/usr/local/opt/llvm/bin/llvm-objcopy', required: false)
have_rust_native = false
# Declared out here because the -D that turns on the registry entry is decided
# outside the block that sets it, and meson has no notion of an unset variable.
have_helix = false
if get_option('native_rust') != 'disabled'
    if cargo.found() and llvm_objcopy.found() and host_machine.system() in ['darwin', 'ios']
        have_rust_native = true
    elif get_option('native_rust') == 'enabled'
        error('native_rust=enabled but cargo or llvm-objcopy is missing')
    elif get_option('native_helix').enabled()
        error('native_helix needs the Rust toolchain: cargo and llvm-objcopy')
    endif
endif

have_curl_shim = false
if host_machine.system() in ['darwin', 'ios']
    have_curl_shim = add_languages('objc', required: false, native: false)
endif
if not have_curl_shim and host_machine.system() in ['darwin', 'ios']
    warning('no Objective-C compiler -- curl and wget will be stubs')
endif
dependencies = [librt, libm, libdl, libatomic, threads, sqlite3]
if zlib.found()
    dependencies += zlib
endif
if have_curl_shim
    dependencies += dependency('foundation')
endif

# zsh needs tgetent/tgetstr/tgoto/tputs and, unlike bash, does not bundle a
# termcap of its own. This USED to add -ltermcap (or -lcurses) here, and that
# was the bug kernel/native_termcap.c exists to fix: the system library reads
# the HOST's terminfo database, so on a device -- which has none -- ZLE got no
# capabilities and could not move the cursor. The shim answers those six names
# from the GUEST's database now, so the system library is not merely
# unnecessary, it is the wrong answer. It also takes away the reason the app
# build could not have zsh: the Xcode project links the meson archives by name
# and would have had to grow a libcurses of its own, and there is nothing left
# for it to link (the iOS SDK ships the .tbd stubs but no curses.h/term.h,
# which is also why deps/zsh is configured termcap-only).
if have_zsh
    # iconv, which zsh uses for two things: converting a filename that is
    # not valid in the current charset (Src/utils.c zreaddir) and turning a
    # \u escape into the locale's encoding (ucs4tomb). Darwin and iOS both
    # ship libiconv, so this is a link-line entry rather than a port. The
    # alternative was configuring zsh without it, which silently changes what
    # $'é' produces -- not a thing to decide by omission.
    #
    # Where it LIVES is not the same question as whether it is there. Darwin and
    # iOS ship it as a separate libiconv, which is what this originally assumed;
    # glibc and musl put iconv_open inside libc itself and have no -liconv to
    # link at all. Asking only the first question broke every Linux CI build
    # from e06c2aa5 until this commit -- `ld: cannot find -liconv' -- while the
    # mac job went on passing, because on the mac the assumption was true.
    iconv_lib = cc.find_library('iconv', required: false)
    if iconv_lib.found()
        dependencies += iconv_lib
    elif not cc.has_function('iconv_open', prefix: '#include <iconv.h>')
        error('zsh needs iconv: no libiconv, and no iconv_open in libc either')
    endif
endif

subdir('vdso') # ish depends on the vdso

offsets = custom_target('offsets',
    output: 'cpu-offsets.h', input: 'jit/offsets.c', depfile: 'cpu-offsets.h.d',
    command: [find_program('tools/staticdefine.sh'), '@OUTDIR@/compile_commands.json', '@INPUT@', '@OUTPUT@', '@DEPFILE@'])

# Generate the embedded /AOK/tests table (consumed by fs/aok.c) from the curated
# fs/aok-tests.manifest, so the regression sources under tests/manual/ ship in
# /AOK/tests automatically. Editing a listed test, or adding a manifest line,
# propagates on the next build. The manifest is read at configure time so every
# listed source is a proper build dependency.
fs_mod = import('fs')
python = find_program('python3')
aok_test_srcs = []
foreach line : fs_mod.read('fs/aok-tests.manifest').strip().split('\n')
    entry = line.strip()
    if entry != '' and not entry.startswith('#')
        aok_test_srcs += files('tests/manual' / entry.split()[0])
    endif
endforeach
aok_generated_tests = custom_target('aok_generated_tests',
    output: 'aok_generated_tests.inc',
    input: [files('tools/gen-aokfs.py', 'fs/aok-tests.manifest')] + aok_test_srcs,
    command: [python, '@INPUT0@', '@INPUT1@',
              meson.current_source_dir() / 'tests' / 'manual', '@OUTPUT@'])

# Same mechanism for the /AOK/tools table, from fs/aok-tools.manifest with
# sources under opt/AOK/tools/. Files are served read-only at /AOK/tools/<name>.
aok_tool_srcs = []
foreach line : fs_mod.read('fs/aok-tools.manifest').strip().split('\n')
    entry = line.strip()
    if entry != '' and not entry.startswith('#')
        aok_tool_srcs += files('opt/AOK/tools' / entry.split()[0])
    endif
endforeach
aok_generated_tools = custom_target('aok_generated_tools',
    output: 'aok_generated_tools.inc',
    input: [files('tools/gen-aokfs.py', 'fs/aok-tools.manifest')] + aok_tool_srcs,
    command: [python, '@INPUT0@', '@INPUT1@',
              meson.current_source_dir() / 'opt' / 'AOK' / 'tools', '@OUTPUT@',
              '/tools', '_tools'])

# Same mechanism for the /AOK/docs table, from fs/aok-docs.manifest with
# sources under opt/AOK/docs/. Files are served read-only at /AOK/docs/<name>.
aok_doc_srcs = []
foreach line : fs_mod.read('fs/aok-docs.manifest').strip().split('\n')
    entry = line.strip()
    if entry != '' and not entry.startswith('#')
        aok_doc_srcs += files('opt/AOK/docs' / entry.split()[0])
    endif
endforeach
aok_generated_docs = custom_target('aok_generated_docs',
    output: 'aok_generated_docs.inc',
    input: [files('tools/gen-aokfs.py', 'fs/aok-docs.manifest')] + aok_doc_srcs,
    command: [python, '@INPUT0@', '@INPUT1@',
              meson.current_source_dir() / 'opt' / 'AOK' / 'docs', '@OUTPUT@',
              '/docs', '_docs'])

# The syscall numbers a native program may issue, generated from calls.c's own
# table so the two cannot drift. The hand-written list this replaces named 77
# of the 217 syscalls AOK implements -- "only what the shim uses" -- which is
# the same shape of gap tools/check-native-libc.py exists to close, and it left
# bash with 140 kernel calls it could not reach for no reason but that nobody
# had needed them yet.
native_syscall_nums = custom_target('native_syscall_nums',
    output: 'native_syscall_nums.h',
    input: files('tools/gen-native-syscalls.py', 'kernel/calls.c'),
    command: [python, '@INPUT0@', '@INPUT1@', '@OUTPUT@'])

emu_src = [
    'emu/tlb.c',
    'emu/fpu.c',
    'emu/vec.c',
    'emu/avx.c', # arch-independent AVX/VEX semantics, shared by the amd64 and i386 front-ends
    'emu/mmx.c',
    'emu/float80.c',
    'emu/amd64_interp.c', # always built: host gadgets + jit/gen.c link its amd64_jit_* helpers
    'jit/jit.c',
    'jit/gen.c',
    'jit/hle.c',
    'jit/helpers.c',
    'jit/riscv64_vendor_ext.c', # portable C, inert unless gen_step_riscv64 ever runs
]
gadgets = 'jit/gadgets-' + jit_cpu_family # resolved at the top of this file
emu_src += [
    gadgets+'/entry.S',
    gadgets+'/memory.S',
    gadgets+'/control.S',
    gadgets+'/math.S',
    gadgets+'/bits.S',
    gadgets+'/string.S',
    gadgets+'/misc.S',
    offsets,
]
# AArch64 GUEST gadgets (native ARM64 guest code) -- only meaningful/
# buildable on an aarch64 host, since they're literal AArch64 assembly.
# Distinct from gadgets-aarch64/ above (the ARM64 HOST gadget set used by
# the i386/amd64 guest JIT) -- see aarch64_guest_plan.md's naming note.
if guest_archs.contains('arm64')
    emu_src += ['emu/arm64_interp.c']
endif
if jit_cpu_family == 'aarch64' and guest_archs.contains('arm64')
    emu_src += [
        'jit/guest-arm64/math.S',
        'jit/guest-arm64/control.S',
        'jit/guest-arm64/memory.S',
        'jit/guest-arm64/logical.S',
        'jit/guest-arm64/dpreg.S',
        'jit/guest-arm64/atomics.S',
        'jit/guest-arm64/dpextra.S',
        'jit/guest-arm64/simd.S',
        'jit/guest-arm64/simd_arith.S',
        'jit/guest-arm64/simd_misc.S',
        'jit/guest-arm64/simd_shift.S',
        'jit/guest-arm64/crypto.S',
        'jit/guest-arm64/fp.S',
    ]
endif
# RISC-V GUEST gadgets: AArch64 host assembly implementing riscv64 guest
# semantics (jit/guest-riscv64/gadgets.h header comment).
if jit_cpu_family == 'aarch64' and guest_archs.contains('riscv64')
    emu_src += [
        'jit/guest-riscv64/core.S',
        'jit/guest-riscv64/alu.S',
        'jit/guest-riscv64/memory.S',
        'jit/guest-riscv64/atomics.S',
        'jit/guest-riscv64/fp.S',
    ]
endif

libish_emu = library('ish_emu', emu_src, include_directories: includes)

libfakefs = library('fakefs',
    ['fs/fake-db.c', 'fs/fake-migrate.c', 'fs/fake-rebuild.c', 'fs/fake-conn.c',
     'util/lockstats.c'],
    include_directories: includes,
    dependencies: sqlite3)

subdir('deps')

if get_option('kernel') == 'ish'
    src = [
        'kernel/init.c',
        'kernel/errno.c',

        'kernel/calls.c',
        'kernel/ish_accel_crypto.c',
        'kernel/ish_accel_aes.c',
        'kernel/ish_accel.c',
        'kernel/ish_accel_pix.c',
        'kernel/ish_accel_pix_kernels.c',
        'kernel/user.c',
        'kernel/vdso.c', vdso,
        'kernel/task.c',
        'kernel/group.c',
        'kernel/log.c',

        'kernel/fork.c',
        'kernel/exec.c',
        'kernel/native.c',
        'kernel/native_io.c',
        'kernel/native_libc.c',
        'kernel/native_kqueue.c',
        'kernel/native_termcap.c',
        'kernel/native_syscall.c', native_syscall_nums,
        'kernel/smallclue_glue.c',
        'kernel/openssh_glue.c',
        'kernel/exit.c',
        'kernel/time.c',
        'kernel/mmap.c',
        'kernel/uname.c',
        'kernel/tls.c',
        'kernel/futex.c',
        'kernel/getset.c',
        'kernel/signal.c',
        'kernel/resource.c',
        'kernel/random.c',
        'kernel/misc.c',
        'kernel/eventfd.c',
        'kernel/pidfd.c',
        'kernel/inotify.c',
        'kernel/ipc.c',
        'kernel/sysvmsg.c',
        'kernel/sysvsem.c',
        'kernel/memfd.c',
        'kernel/ptrace.c',

        'kernel/fs.c',
        'kernel/fs_info.c',
        'fs/aok.c', aok_generated_tests, aok_generated_tools, aok_generated_docs,
        'fs/mount.c',
        'fs/fd.c',
        'fs/inode.c',
        'fs/stat.c',
        'fs/dir.c',
        'fs/generic.c',
        'fs/path.c',
        'fs/net_route.c',
        'fs/real.c',
        'fs/fake.c',
        'fs/mmap_cache.c',

        'fs/proc.c',
        'fs/proc/entry.c',
        'fs/proc/ish.c',
        'fs/proc/net.c',
        'fs/proc/sys.c',
        'fs/proc/root.c',
        'fs/proc/pid.c',

        'fs/dyndev.c',

        'fs/adhoc.c',
        'fs/sock.c',
        'fs/pipe.c',
        'fs/fifo.c',
        'fs/sockrestart.c',
        'fs/lock.c',

        'fs/dev.c',
        'fs/mem.c',
        'fs/tty.c',
        'fs/tty-real.c',
        'fs/pty.c',
        'fs/tmp.c',

        'fs/poll.c',
        'kernel/poll.c',
        'kernel/epoll.c',

        'util/timer.c',
        'util/ro_locks.c',
        'util/rw_locks.c',
        'util/sync.c',
        'util/fifo.c',

        'emu/memory.c',
        'platform/standalone.c',

        'platform/' + host_machine.system() + '.c',
    ]

    # SmallCLUE, compiled in as HOST code so its applets run without being
    # translated (kernel/native.h). Two things make this unlike an ordinary
    # vendored dependency:
    #
    #  - src/main.c is excluded: it is a four-line shim around smallclueMain,
    #    and its main() is the one symbol that collides with AOK's.
    #  - Every file is force-included with kernel/native_libc.h, which
    #    redirects the libc surface onto AOK's VFS. Without it a plain open()
    #    in an applet resolves against the *iOS* filesystem and quietly reads
    #    the wrong file; tools/check-native-libc.py is the gate that keeps
    #    that honest.
    #
    # Excluded feature groups, entry points stubbed in kernel/smallclue_glue.c:
    # integration.c is the WITH_EXSH path into PSCAL's VM.
    smallclue_excluded = ['main.c', 'integration.c']
    # AOK's own sources compiled into libsmallclue, so that they are built with
    # smallclue_c_args and therefore with kernel/native_libc.h force-included.
    smallclue_extra_src = []

    # gzip_app and tar_app were excluded for a real reason, and it was never the
    # link line. zlib is a system dylib built against the HOST libc, so the gz*
    # file API does host I/O on whatever path or descriptor it is handed --
    # invisibly correct wherever a path exists on both sides, and quietly
    # operating on iOS's filesystem the whole time. See
    # deps/smallclue-shim/zlib.h, which reimplements those six calls over the
    # redirected open/read/write and leaves the compression itself to zlib,
    # whose deflate/inflate touch nothing but the caller's buffers.
    if zlib.found()
        smallclue_extra_src += files('deps/smallclue-shim/zlib_guest.c')
    else
        smallclue_excluded += ['gzip_app.c', 'tar_app.c']
        warning('zlib not found -- tar and gzip will be stubs')
    endif

    # checksum_app.c (md5sum/sha1sum/sha256sum) was on that list too, for the
    # same shape of reason and with a better answer: it wants OpenSSL's EVP,
    # SmallCLUE has OpenSSL because libgit2 and OpenSSH pull it in, and AOK
    # links neither -- so three working commands were stubbed over one
    # #include. deps/smallclue-shim/openssl/evp.h supplies that #include from
    # CommonCrypto, which is in libSystem on macOS and iOS, so unlike zlib this
    # needs nothing added to the Xcode project.
    #
    # Apple-only on purpose: elsewhere there is no CommonCrypto, and the honest
    # stub is better than a build that fails at the include.
    have_checksums = host_machine.system() in ['darwin', 'ios']
    if not have_checksums
        smallclue_excluded += 'checksum_app.c'
    endif

    # --------------------------------------------------------------- openssh
    #
    # ssh/scp/sftp/ssh-keygen/ssh-copy-id, from the OpenSSH tree vendored at
    # deps/smallclue/third-party/openssh. Like Nextvi it is a tree a plain
    # checkout does not populate, so the build works either way and says so.
    #
    # Two things have to be true before it can be built, and they are checked
    # separately because the fixes differ:
    #
    #  - the tree is there at all (ssh.c), and
    #  - it has been CONFIGURED (config.h), with:
    #
    #        cd deps/smallclue/third-party/openssh
    #        ./configure --without-openssl --without-zlib --sysconfdir=/etc/ssh
    #
    # --without-openssl is not a shortcut, it is the whole reason this links:
    # AOK ships no libcrypto and no zlib, and the Xcode app targets link the
    # meson archives by name with nothing else on the line. Configured that way
    # OpenSSH falls back to its own bundled crypto and the link line is just
    # "-lresolv" -- no new libraries and no change to the app project.
    #
    # The price, and it is a real one someone will hit: without libcrypto the
    # client supports ED25519 KEYS AND chacha20-poly1305 ONLY. RSA and ECDSA
    # keys -- including most keys people already have -- will not work, and
    # `ssh-keygen -t rsa` will refuse. That is a deliberate trade, not a bug.
    #
    # The tree also has to be the PATCHED one: smallclue's
    # fetch_dependencies.sh renames ssh.c/scp.c/sftp.c/ssh-keygen.c's main()
    # to pscal_openssh_*_main() with sed, which is what
    # deps/smallclue/src/openssh_app.c calls. An unpatched tree links four
    # main()s against AOK's own.
    openssh_dir = 'deps/smallclue/third-party/openssh'
    have_openssh_tree = run_command(python, '-c',
            'import os, sys; sys.exit(0 if os.path.exists(sys.argv[1]) else 1)',
            meson.current_source_dir() / openssh_dir / 'ssh.c',
            check: false).returncode() == 0
    have_openssh = have_openssh_tree and run_command(python, '-c',
            'import os, sys; sys.exit(0 if os.path.exists(sys.argv[1]) else 1)',
            meson.current_source_dir() / openssh_dir / 'config.h',
            check: false).returncode() == 0

    if not have_openssh
        # src/openssh_app.c is the launcher (setjmp around fatal()/exit(), pty
        # handling, the re-entry split); openssh_shim.c and openssh_stubs.c only
        # exist to feed it. None of them compile without the tree's headers, so
        # they drop out together and kernel/smallclue_glue.c's honest "not built
        # into this iSH-AOK" stubs stay.
        smallclue_excluded += ['openssh_app.c', 'openssh_shim.c',
                               'openssh_stubs.c']
        if have_openssh_tree
            warning('deps/smallclue/third-party/openssh is present but has no ' +
                    'config.h -- ssh/scp/sftp/ssh-keygen will report "not ' +
                    'built into this iSH-AOK". Configure it with: ./configure ' +
                    '--without-openssl --without-zlib --sysconfdir=/etc/ssh')
        else
            warning('deps/smallclue/third-party/openssh is not checked out -- ' +
                    'ssh/scp/sftp/ssh-keygen will report "not built into this ' +
                    'iSH-AOK"')
        endif
    else
        # src/openssh_stubs.c does two jobs, and only one of them survives
        # contact with a real OpenSSH: it implements the exit-context helpers
        # openssh_app.c calls, AND it defines four WEAK pscal_openssh_*_main()
        # that print "OpenSSH integration unavailable in this build".
        #
        # Weak loses to strong within one link -- but NOT across static
        # archives. The linker takes the first archive member that defines a
        # symbol and stops looking, so the stub satisfied openssh_app.c and
        # ssh.o was never pulled in at all: `ssh -V` printed "OpenSSH
        # integration unavailable" out of a binary that contained every line of
        # OpenSSH. So the file is compiled on its own below, with those four
        # names renamed out of the way rather than left to lose a race.
        smallclue_excluded += 'openssh_stubs.c'
    endif

    # Nextvi -- the editor `vi` runs -- is a submodule OF a submodule and is not
    # populated by a plain checkout, so the build has to work either way. With
    # the tree present its single translation unit is compiled in (vi.c
    # #includes the other eight sources) and src/nextvi_stubs.c drops out; with
    # it absent the stub's weak nextvi_main_entry prints "editor unavailable in
    # this build" and `vi` exits 127. Populate it with:
    #
    #     git -C deps/smallclue submodule update --init third-party/nextvi
    #
    # -- and NOT a bare `--init`, which also drags in the abandoned deps/linux.
    # The result is fixed at configure time, so populating the submodule
    # afterwards needs a `meson setup --reconfigure` before ninja sees it.
    nextvi_dir = 'deps/smallclue/third-party/nextvi'
    have_nextvi = run_command(python, '-c',
            'import os, sys; sys.exit(0 if os.path.exists(sys.argv[1]) else 1)',
            meson.current_source_dir() / nextvi_dir / 'vi.c',
            check: false).returncode() == 0

    # Nextvi runs `!!` and `:!cmd` with fork+dup2+execvp, and a native program
    # cannot fork. Upstream Nextvi therefore reports "fork failed" for the shell
    # escape; a Nextvi carrying the NEXTVI_PLATFORM_SPAWN seam in term.c calls
    # kernel/nextvi_glue.c's nextvi_platform_spawn instead and it works.
    #
    # Detected rather than assumed, so that both are correct builds: without
    # the seam nothing here is compiled in and the escape fails honestly, and
    # nothing has to be remembered when the submodule moves either way.
    have_nextvi_spawn = have_nextvi and run_command(python, '-c',
            'import sys; sys.exit(0 if "NEXTVI_PLATFORM_SPAWN" in open(sys.argv[1]).read() else 1)',
            meson.current_source_dir() / nextvi_dir / 'term.c',
            check: false).returncode() == 0

    smallclue_src = []
    if have_nextvi
        smallclue_excluded += 'nextvi_stubs.c'
        # Supplies nextvi_main_entry over the renamed real one; see the file.
        src += files('kernel/nextvi_glue.c')
        if not have_nextvi_spawn
            warning('this Nextvi has no NEXTVI_PLATFORM_SPAWN seam in term.c, ' +
                    'so `!!` and `:!cmd` inside vi will report "fork failed"')
        endif
    else
        # Loudly, because the runtime symptom is not: the stub's diagnostic is
        # written and then erased by the terminal reset SmallCLUE emits on the
        # way out, and the editor's status is discarded, so `vi` on a stub build
        # simply vanishes and reports success. That was reported from a device
        # as "vi exits immediately", and it took a build to reproduce.
        warning('nextvi is not checked out -- `vi` will be a stub that exits ' +
                'silently. Populate it with: git -C deps/smallclue submodule ' +
                'update --init third-party/nextvi')
    endif
    # Space-separated, not newline: meson interprets \n inside its own string
    # literals, which would break the one-liner before python ever sees it.
    foreach f : run_command(python, '-c',
            'import glob, os, sys; print(" ".join(sorted(os.path.basename(p) for p in glob.glob(sys.argv[1] + "/src/*.c"))))',
            meson.current_source_dir() / 'deps/smallclue',
            check: true).stdout().strip().split(' ')
        if f != '' and not smallclue_excluded.contains(f)
            smallclue_src += files('deps/smallclue/src' / f)
        endif
    endforeach

    smallclue_includes = include_directories(
        'deps/smallclue-shim',
        # Headers that exist on Darwin and not here. Gated so the shipping
        # build never picks up a stand-in for a header it really has.
        host_machine.system() in ['darwin', 'ios'] ? [] : 'deps/smallclue-shim/linux',
        'deps/smallclue',
        'deps/smallclue/src',
        'deps/smallclue/common',
        'deps/smallclue/compat',
    )

    smallclue_c_args = [
        '-include', meson.current_source_dir() / 'kernel/native_libc.h',
        '-DSMALLCLUE_PLATFORM_SPAWN',
        # SmallCLUE is written against BSD/Darwin header visibility, where
        # ptsname_r, strptime and FNM_CASEFOLD are simply there. glibc puts all
        # three behind feature-test macros, so on Linux they vanish and the
        # compiler reports them as implicit declarations -- with strptime that
        # then cascades into an int-to-pointer error, because an undeclared
        # function is assumed to return int.
        #
        # Darwin ignores the macro, so this is unconditional rather than
        # host-gated. It has to be a -D and not a #define in a header: the macro
        # only counts if it precedes the first libc header, and the force-include
        # above is itself a header.
        #
        # Not noticed until 21a2a4a8, because until then no Linux build got past
        # the iconv probe at configure time to compile any of this.
        '-D_GNU_SOURCE',
        # dmesg's real body sits behind a __linux__ test, which is false for a
        # native program (compiled for the HOST) even though the kernel it talks
        # to IS Linux -- AOK's own. The shim routes klogctl to the guest's
        # syslog(2), so say so rather than letting the #else claim dmesg is
        # unsupported on a system whose /usr/bin/dmesg works.
        '-DSMALLCLUE_HAVE_KLOGCTL',
        # `md` with no arguments lists the documents in this directory, and
        # `md native-programs` resolves a bare name against it. The default is
        # $HOME/Docs, which an AOK guest has no reason to have -- so md looked
        # broken rather than empty. /AOK/docs is compiled into the app
        # (fs/aok-docs.manifest) and is always there.
        '-DSMALLCLUE_MARKDOWN_DOCS_DIR="/AOK/docs"',
        '-w',  # vendored code, not ours to police
    ]

    # core.c reaches for <curl/curl.h> under this, and gets
    # deps/smallclue-shim/curl/curl.h -- which is on the include path above,
    # ahead of the SDK's own on the one platform that has one.
    if have_curl_shim
        smallclue_c_args += '-DPSCAL_HAS_LIBCURL'
    endif

    # src/openssh_app.c and src/openssh_shim.c reach into the vendored tree
    # (sshkey.h, pscal_runtime_hooks.h), so libsmallclue needs its headers too
    # -- but only when it is there, since include_directories() on a missing
    # directory is a configure error.
    openssh_includes = []
    if have_openssh
        openssh_includes = [include_directories(
            openssh_dir, openssh_dir / 'openbsd-compat')]
    endif

    libsmallclue = static_library('smallclue', smallclue_src + smallclue_extra_src,
        include_directories: [includes, smallclue_includes] + openssh_includes,
        c_args: smallclue_c_args)

    # NOT compiled with the native_libc.h force-include the rest of SmallCLUE
    # gets. This file's whole job is to talk to Foundation, so redirecting its
    # libc onto the guest would be redirecting the wrong half -- the bytes go
    # back to the caller's write callback, which IS redirected, and that is
    # where the guest belongs in this picture. deps/smallclue-shim/curl/curl.h
    # says the rest.
    # The Rust native program: cargo, then the objcopy rename, in one step.
    # See tools/build-rust-native.sh for why they cannot be separated.
    librustnative = []
    if have_rust_native
        nlibc_renames = custom_target('nlibc-renames',
            input: 'kernel/native_libc.h',
            output: 'nlibc-renames.txt',
            command: [find_program('tools/gen-nlibc-renames.py'), '@INPUT@'],
            capture: true)
        # helix is a cargo FEATURE of the one native-Rust crate, not a crate
        # of its own. Each staticlib carries its own std, so two of them in a
        # link is thousands of duplicate symbols -- which is what happened the
        # first time this was two crates.
        native_features = get_option('native_rust_features')
        if get_option('native_helix').enabled()
            if not fs_mod.exists('deps/helix/helix-term/Cargo.toml')
                error('native_helix: deps/helix is empty -- git submodule update --init deps/helix')
            endif
            have_helix = true
            native_features = native_features == '' ? 'helix' : native_features + ',helix'
        endif

        rust_native_lib = custom_target('rust-native',
            input: nlibc_renames,
            output: 'aok_native_routed.o',
            command: [find_program('tools/build-rust-native.sh'),
                      meson.current_source_dir() / 'deps/rust-native',
                      '@OUTPUT@', cargo, llvm_objcopy, '@INPUT@',
                      # Empty for a host build; the Xcode path passes the iOS
                      # triple, because a crate built for macOS would link but
                      # target the wrong platform.
                      get_option('native_rust_target'),
                      # Part of the command line rather than only the
                      # environment, so ninja notices a change to it. Through
                      # the environment alone the option could be flipped and
                      # yesterday's crate would ship.
                      native_features],
            # Without this the crate's own sources are invisible to ninja --
            # cargo would rebuild them but the target is only rerun when the
            # rename list changes, so an edit to the Rust silently ships the
            # previous binary. Caught exactly that way.
            depend_files: files('deps/rust-native/src/lib.rs',
                                'deps/rust-native/src/probe.rs',
                                'deps/rust-native/src/helix.rs',
                                'deps/rust-native/Cargo.toml'),
            build_by_default: true)
        # One object, added to libish's own objects, rather than a second
        # archive next to it. The Xcode build does not link through meson --
        # it links -lish against the archive meson produced -- so anything
        # carried on a meson dependency's link_args reaches the CLI build and
        # nothing else. Inside libish.a it needs no cooperation from either
        # build system.
        librustnative = [rust_native_lib]
    endif

    libcurlshim = []
    if have_curl_shim
        libcurlshim = [static_library('curlshim',
            files('deps/smallclue-shim/curl_nsurlsession.m'),
            include_directories: [includes, smallclue_includes],
            objc_args: ['-fobjc-arc'])]
    endif

    # Nextvi gets an archive of its own for one reason: a -D that must apply to
    # its translation unit and to no other. It is a program written to run once
    # and exit, and AOK runs it again every time someone types `vi`, so its
    # entry point is renamed and kernel/nextvi_glue.c supplies a
    # nextvi_main_entry that resets the state a fresh process would have had.
    # Renaming through the compiler is how SmallCLUE's own CMake build already
    # turns Nextvi's main() into nextvi_main_entry.
    libnextvi = []
    if have_nextvi
        nextvi_c_args = smallclue_c_args + ['-Dnextvi_main_entry=nextvi_editor_run']
        if have_nextvi_spawn
            nextvi_c_args += '-DNEXTVI_PLATFORM_SPAWN'
        endif
        libnextvi = static_library('nextvi', files(nextvi_dir / 'vi.c'),
            include_directories: [includes, smallclue_includes],
            c_args: nextvi_c_args)
    endif

    # --------------------------------------------------------------- openssh
    #
    # OpenSSH is not built as a library upstream: it is five programs that
    # share a pile of .c files, and the Makefile links a different subset into
    # each. AOK links ONE binary, so the subset has to be chosen here -- and
    # the choice is not obvious, because "client" and "server" are not
    # separated by filename. What follows is ported from SmallCLUE's own
    # CMakeLists.txt, where it was derived one link error at a time; the
    # reasons are kept because re-deriving them costs a day.
    #
    # Start from every .c in the tree, then drop, in groups:
    #
    #  - Other programs. ssh-agent, ssh-add, ssh-keyscan, ssh-keysign,
    #    ssh-sk-helper, ssh-pkcs11-helper, sftp-server(-main) and sshd* each
    #    have their own main(); only ssh/scp/sftp/ssh-keygen got the
    #    main() -> pscal_openssh_*_main() rename, so the rest collide with
    #    AOK's main() at link time. contrib/ is GUI askpass helpers (also with
    #    their own main()), regress/ is the test suite.
    #
    #  - The server. auth2-*, auth-passwd/rhosts/shadow, monitor*, servconf,
    #    session, serverloop, sshlogin, loginrec/logintest, sandbox-*: sshd's
    #    privsep monitor, its config parser, its select loop, its utmp/wtmp
    #    accounting, its sandbox. auth.c/auth2.c/sshlogin.c/serverloop.c have
    #    generic-sounding names and were missed by the patterns above -- they
    #    are server-only all the same, and once their own callees are gone they
    #    fail to link. No client source calls any of them.
    #
    #  - Duplicate symbols, which is where the surprises live:
    #      * ssh-pkcs11.c is the dlopen-based provider meant to run INSIDE
    #        ssh-pkcs11-helper; the client side is ssh-pkcs11-client.c, which
    #        talks to it over IPC. Both define pkcs11_add_provider and friends.
    #        Same story for ssh-sk-client.c against ssh-sk.c.
    #      * cleanup.c's cleanup_exit() is the trivial default (_exit(i));
    #        clientloop.c defines a client-specific one that restores the
    #        terminal and kills the mux proxy, and no-ops those safely when ssh
    #        never started them. One binary, one cleanup_exit: keep the fuller
    #        one. scp.c has a THIRD (closing remin/remout) -- that one is
    #        renamed rather than dropped, below.
    #      * sftp-realpath.c's sftp_realpath(path, resolved) resolves a LOCAL
    #        path for the sftp *server*; sftp-client.c's sftp_realpath(conn,
    #        path) sends a REALPATH request to the remote. The client wants the
    #        second.
    #      * dns.c (VerifyHostKeyDNS via SSHFP records) collides with
    #        src/openssh_shim.c's deliberate verify_host_key_dns() stub, and
    #        wants getrrsetbyname() besides. SSHFP lookups stay off, so dns.c
    #        and getrrsetbyname*.c both go rather than the stub.
    #
    # Three files the broad patterns catch by accident and the client needs:
    # monitor_fdpass.c (fd passing, used by ssh's mux), sshsig.c (ssh-keygen
    # -Y), platform-pledge.c (a no-op pledge() on everything but OpenBSD).
    openssh_excluded = [
        # own main()
        'ssh-agent.c', 'ssh-add.c', 'ssh-keyscan.c', 'ssh-keysign.c',
        'ssh-sk-helper.c', 'ssh-pkcs11-helper.c',
        'sftp-server.c', 'sftp-server-main.c',
        # server
        'auth.c', 'auth2.c', 'auth-passwd.c', 'auth-rhosts.c', 'auth-shadow.c',
        'servconf.c', 'session.c', 'serverloop.c', 'sshlogin.c',
        'loginrec.c', 'logintest.c', 'gss-genr.c', 'platform-tracing.c',
        # duplicate symbols
        'ssh-pkcs11.c', 'ssh-sk-client.c', 'cleanup.c', 'sftp-realpath.c',
        'dns.c',
        # reaches the host, and cannot be redirected any other way.
        #
        # mktemp.c exists in this build only to define _ssh_mkstemp(), the
        # umask-clamping wrapper that openbsd-compat.h's UNCONDITIONAL
        # `#define mkstemp(x) _ssh_mkstemp(x)` points every caller at
        # (everything else in the file is behind !HAVE_MKDTEMP, which Darwin
        # has). That macro lands AFTER the force-included native_libc.h and
        # overwrites its `#define mkstemp nlibc_mkstemp`, and then mktemp.c
        # opens with `#ifdef mkstemp / #undef mkstemp / int mkstemp(char *);`
        # -- deliberately undoing any redirection, including a -D on the
        # command line. So the routed mkstemp was quietly bypassed and
        # hostfile.c and ssh-keygen.c created their temp files on the DEVICE,
        # with the host fd number then handed to the routed fchmod/write/close
        # as if it were a guest descriptor.
        #
        # Dropping the file is what makes the redirection stick: nothing else
        # defines _ssh_mkstemp, so native_libc.h's `#define _ssh_mkstemp
        # nlibc_mkstemp` catches the rescan of openbsd-compat.h's macro and
        # every call site lands on the guest's mkstemp -- which already applies
        # the 0600 the wrapper's umask dance was for.
        'mktemp.c',
    ]
    # Whole families, matched on the leading name: auth2-* and sandbox-* and
    # sshd* are server; monitor* is the privsep monitor (minus monitor_fdpass);
    # getrrsetbyname* is dns.c's dependency.
    openssh_excluded_prefixes = ['auth2-', 'sandbox-', 'sshd', 'monitor',
                                 'getrrsetbyname']
    openssh_readded = ['monitor_fdpass.c', 'sshsig.c', 'platform-pledge.c']

    # Two sources need a compiler flag that must not reach any other, so each
    # is compiled into a one-file archive of its own. Same reason Nextvi has its
    # own: meson can only vary flags per target.
    #
    #  - scp.c defines a THIRD cleanup_exit(), closing the four remote fds it
    #    owns, unrelated to the clientloop.c one everything else resolves to.
    #    Renaming it is scoped here so scp's own call sites -- the only callers
    #    -- move with it.
    #  - smult_curve25519_ref.c has a file-static helper called select(), which
    #    native_libc.h's `#define select nlibc_select` turns into a second,
    #    wrongly-typed declaration of nlibc_select. A -D cannot undo that (the
    #    header is included after the command line), so a second force-included
    #    header undefines it afterwards. See that header for the rule.
    openssh_special = {
        'scp.c': ['-Dcleanup_exit=pscal_openssh_scp_cleanup_exit'],
        'smult_curve25519_ref.c': ['-include',
            meson.current_source_dir() / 'deps/smallclue-shim/openssh_local_names.h'],
    }

    libopenssh = []
    libopenssh_special = []
    if have_openssh
        openssh_src = []
        openssh_special_src = {}
        # Paths relative to the tree, so the directory tests below can see
        # contrib/ and regress/. Space-separated for the same reason as the
        # SmallCLUE glob above: meson eats \n in its own string literals.
        foreach p : run_command(python, '-c',
                'import glob, sys; print(" ".join(sorted(p[len(sys.argv[1]) + 1:] for p in glob.glob(sys.argv[1] + "/**/*.c", recursive=True))))',
                meson.current_source_dir() / openssh_dir,
                check: true).stdout().strip().split(' ')
            if p == ''
                continue
            endif
            base = p.split('/')[-1]
            keep = true
            if p.contains('contrib/') or p.contains('regress/')
                keep = false
            elif openssh_excluded.contains(base)
                keep = false
            else
                foreach prefix : openssh_excluded_prefixes
                    if base.startswith(prefix)
                        keep = false
                    endif
                endforeach
            endif
            if openssh_readded.contains(base)
                keep = true
            endif
            if keep
                if openssh_special.has_key(base)
                    openssh_special_src += {base: files(openssh_dir / p)}
                else
                    openssh_src += files(openssh_dir / p)
                endif
            endif
        endforeach

        # OpenSSH brings its own glob (openbsd-compat/glob.c) and has to: sftp
        # globs REMOTE paths through GLOB_ALTDIRFUNC, handing glob() its own
        # opendir/readdir/stat that speak the SFTP protocol. Routing glob() to
        # nlibc_glob would both collide with that definition and, if it linked,
        # glob the local filesystem for a remote pattern. The compat glob calls
        # the ordinary directory functions underneath, and those are routed, so
        # local globbing still sees the guest. Same escape hatch bash uses --
        # see kernel/native_libc.h.
        #
        # Two names collide with the native bash in the same binary, out of the
        # ~170 translation units on each side. Renaming OpenSSH's rather than
        # bash's keeps each rename inside one archive: these two are declared in
        # OpenSSH's own headers and used only by OpenSSH's own sources, and none
        # of deps/smallclue/src reaches for either.
        #  - xmalloc: both are "malloc or die", but bash's is the one its whole
        #    tree and its readline are built around.
        #  - tilde_expand: OpenSSH's takes a const char *, bash's does not, and
        #    they expand different syntax.
        openssh_c_args = smallclue_c_args + [
            '-DNATIVE_LIBC_OWN_GLOB',
            '-Dxmalloc=pscal_openssh_xmalloc',
            '-Dtilde_expand=pscal_openssh_tilde_expand',
        ]
        # implicit_include_directories: false, and it matters. Meson otherwise
        # puts the target's build dir and the PROJECT SOURCE ROOT ahead of
        # every -I given here -- and AOK's root has a misc.h, as does OpenSSH.
        # A quoted #include "misc.h" from a source under openbsd-compat/ does
        # not find its own directory, so it walked the -I list and read AOK's,
        # leaving SSH_TUNMODE_* undeclared in port-net.c with no hint that the
        # wrong header had been used. Nothing here wants AOK's headers anyway:
        # the one AOK file these sources see is kernel/native_libc.h, which is
        # force-included by absolute path and includes only system headers.
        libopenssh = static_library('openssh', openssh_src,
            include_directories: openssh_includes + [includes, smallclue_includes],
            implicit_include_directories: false,
            c_args: openssh_c_args)

        # See the note beside smallclue_excluded above: the four weak
        # pscal_openssh_*_main() here would otherwise beat the real ssh.c,
        # scp.c, sftp.c and ssh-keygen.c to the link.
        libopenssh_special += static_library('openssh_stubs',
            files('deps/smallclue/src/openssh_stubs.c'),
            include_directories: [includes, smallclue_includes] + openssh_includes,
            c_args: smallclue_c_args + [
                '-Dpscal_openssh_ssh_main=pscal_openssh_ssh_main_unavailable',
                '-Dpscal_openssh_scp_main=pscal_openssh_scp_main_unavailable',
                '-Dpscal_openssh_sftp_main=pscal_openssh_sftp_main_unavailable',
                '-Dpscal_openssh_ssh_keygen_main=pscal_openssh_ssh_keygen_main_unavailable',
            ])

        foreach base, extra_args : openssh_special
            if not openssh_special_src.has_key(base)
                # The file was excluded, or the tree moved under us. Silence
                # here would mean scp linking without its cleanup_exit rename.
                error('openssh_special names ' + base + ', which is not in the ' +
                      'source list -- reconcile it with openssh_excluded')
            endif
            libopenssh_special += static_library(
                'openssh_' + base.replace('.c', '').replace('-', '_'),
                openssh_special_src[base],
                include_directories: openssh_includes + [includes, smallclue_includes],
                implicit_include_directories: false,
                c_args: openssh_c_args + extra_args)
        endforeach
    endif

    # ------------------------------------------------------------------ bash
    #
    # GNU bash as a second native program (docs/bash_native_plan.md). Like
    # Nextvi it is a submodule that a plain checkout does not populate, so the
    # build works either way and says so when it does not:
    #
    #     git submodule update --init deps/bash
    bash_dir = 'deps/bash'

    libbash = []
    if have_bash
        bash_includes = include_directories(
            # FIRST, and it holds two headers bash would otherwise find its own
            # copy of: <builtins/builtext.h> and <termcap.h>. See them for why.
            'deps/bash-shim',
            bash_dir, bash_dir / 'generated', bash_dir / 'generated/builtins',
            bash_dir / 'include', bash_dir / 'builtins', bash_dir / 'lib',
            # NOT lib/termcap. Its termcap.h declares the bundled GNU termcap
            # that is no longer built (see the source list below), and having
            # both on the path would leave which one readline compiles against
            # a matter of directory order.
            bash_dir / 'lib/readline',
        )

        # What bash's own Makefile passes. -DHAVE_CONFIG_H above all: without it
        # most of bash does not include config.h at all, and the failures look
        # like missing headers rather than a missing define.
        #
        # CONF_* describe the GUEST rather than this Mac, because they are what
        # $MACHTYPE and $OSTYPE report to a script -- a shell inside AOK saying
        # "darwin" would be lying about the system its scripts are running on.
        bash_defs = [
            '-DHAVE_CONFIG_H', '-DSHELL',
            # NOT -DMACOSX, which bash's configure would set here. It turns on
            # HFS filename normalization (lib/sh/fnxform.c) -- and the
            # filesystem bash sees is the GUEST's fakefs, not the Mac's, so
            # decomposing names the way HFS wants would corrupt them.
            '-DPROGRAM="bash"',
            '-DCONF_HOSTTYPE="' + host_machine.cpu_family() + '"',
            '-DCONF_OSTYPE="linux-gnu"',
            '-DCONF_MACHTYPE="' + host_machine.cpu_family() + '-ish-linux-gnu"',
            '-DCONF_VENDOR="ish"',
            '-DLOCALEDIR="/usr/local/share/locale"', '-DPACKAGE="bash"',
            # The system-wide bashrc, which every distribution's bash is built
            # with and bash's own config-top.h leaves commented out. Without it
            # an interactive native bash silently skips /etc/bash.bashrc, so it
            # comes up with a different prompt, aliases and completion from the
            # guest's own /bin/bash in the same rootfs -- which is precisely the
            # comparison a user makes when deciding whether this shell works.
            '-DSYS_BASHRC="/etc/bash.bashrc"',
            # bash brings its own globbing, and macOS's <glob.h> shares the
            # include guard _GLOB_H_ with bash's -- see kernel/native_libc.h.
            '-DNATIVE_LIBC_OWN_GLOB',
            # NOT -DNATIVE_LIBC_OWN_TERMCAP, which bash carried while it still
            # built its own bundled GNU termcap (lib/termcap). That copy reads
            # /etc/termcap and nothing else, and no rootfs AOK supports ships
            # one -- Debian, Devuan and Alpine all moved to terminfo -- so
            # every capability readline asked for came back empty and its
            # editor erased with a bare space, which ADVANCES the cursor.
            # tgetent and friends route to kernel/native_termcap.c now, which
            # reads the GUEST's terminfo. The bundled files are out of the
            # source list below, so there are no DEFINITIONS left here for the
            # shim to rename into a collision.
            # fork by re-launch (deps/bash/aok_fork.c). Guards the two hooks in
            # bash's own sources so the vendor tree still builds without it.
            '-DAOK_NATIVE_FORK=1',
            # The two symbols that collide with libish, out of 1837 against
            # 1775. main is the obvious one; wait_for is bash's job-control
            # reaper against AOK's condition-variable wait (util/sync.h).
            '-Dmain=bash_main_entry',
            '-Dwait_for=bash_wait_for',
            '-include', meson.current_source_dir() / 'kernel/native_libc.h',
            # Same reason smallclue_c_args carries it: bash reaches for wcswidth,
            # which glibc puts behind a feature-test macro and Darwin exposes by
            # default. Darwin ignores the macro, so it is unconditional.
            '-D_GNU_SOURCE',
            # mkbuiltins writes bash_builtext.h here; deps/bash-shim forwards
            # <builtins/builtext.h> to it.
            '-I' + meson.current_build_dir(),
            '-w',  # vendored code, not ours to police
        ]

        # mkbuiltins turns builtins/*.def into C. It runs on the BUILD machine,
        # so it is a native: true executable and is NOT compiled with the shim
        # -- it is a build tool reading files here, not a program that will run
        # inside the guest.
        mkbuiltins = executable('mkbuiltins',
            files(bash_dir / 'builtins/mkbuiltins.c'),
            native: true,
            include_directories: bash_includes,
            c_args: ['-DHAVE_CONFIG_H', '-DSHELL', '-w'])
        run_mkbuiltins = find_program('tools/run-mkbuiltins.sh')
        bash_def_dir = meson.current_source_dir() / bash_dir / 'builtins'

        bash_defs_files = []
        foreach d : run_command(python, '-c',
                'import glob, os, sys; print(" ".join(sorted(os.path.basename(p) for p in glob.glob(sys.argv[1] + "/*.def"))))',
                bash_def_dir, check: true).stdout().strip().split(' ')
            bash_defs_files += files(bash_dir / 'builtins' / d)
        endforeach

        # The dispatch table and its externs, from every .def at once. Every
        # .def goes in even where the builtin is compiled out: the #if guards
        # inside the generated table are what leave `complete` and friends out
        # of a build without readline.
        bash_builtins_tbl = custom_target('bash_builtins_table',
            output: ['bash_builtins.c', 'bash_builtext.h'],
            input: bash_defs_files,
            command: [run_mkbuiltins, 'table', mkbuiltins, bash_def_dir,
                      '@OUTPUT0@', '@OUTPUT1@', '@INPUT@'])

        # Builtins whose .c must not be LINKED, though their .def still feeds
        # the table above. Each needs readline or history, which this build does
        # not have, and each would drag in rl_*/history_* that nothing defines.
        # bash itself gets away with compiling them because its builtins are a
        # static archive and nothing references them -- link_whole here would
        # pull them in regardless.
        # Every builtin is linked now that readline is in: bind and complete
        # were excluded only because READLINE being off made config-bot.h
        # undefine PROGRAMMABLE_COMPLETION, so the generated table left them
        # out and nothing referred to them.
        bash_builtins_unlinked = [# reserved.def documents the shell keywords
                                  # and produces no C file at all.
                                  'reserved.def']
        bash_builtin_src = []
        foreach d : run_command(python, '-c',
                'import glob, os, sys; print(" ".join(sorted(os.path.basename(p) for p in glob.glob(sys.argv[1] + "/*.def"))))',
                bash_def_dir, check: true).stdout().strip().split(' ')
            if not bash_builtins_unlinked.contains(d)
                bash_builtin_src += custom_target('bash_builtin_' + d.split('.')[0],
                    output: d.replace('.def', '.c'),
                    input: files(bash_dir / 'builtins' / d),
                    command: [run_mkbuiltins, 'one', mkbuiltins, bash_def_dir,
                              '@INPUT@', '@OUTPUT@'])
            endif
        endforeach

        # The sources bash actually links, taken from its own build rather than
        # a glob: several top-level .c are build tools (mksignames, mksyntax),
        # and lib/readline contributes only the nine files bash uses whether or
        # not line editing is compiled in.
        bash_top = ['shell.c', 'eval.c', 'y.tab.c', 'general.c', 'make_cmd.c',
            'print_cmd.c', 'dispose_cmd.c', 'execute_cmd.c', 'variables.c',
            'copy_cmd.c', 'error.c', 'expr.c', 'flags.c', 'jobs.c', 'subst.c',
            'hashcmd.c', 'hashlib.c', 'mailcheck.c', 'trap.c', 'input.c',
            'unwind_prot.c', 'pathexp.c', 'sig.c', 'test.c', 'version.c',
            'alias.c', 'array.c', 'arrayfunc.c', 'assoc.c', 'braces.c',
            'bracecomp.c', 'bashhist.c', 'bashline.c', 'list.c', 'stringlib.c',
            'locale.c', 'findcmd.c', 'redir.c', 'pcomplete.c', 'pcomplib.c',
            'xmalloc.c',
            # iSH-AOK's own: fork by re-launch. Not upstream.
            'aok_fork.c']
        bash_builtins_static = ['common.c', 'evalstring.c', 'evalfile.c',
            'getopt.c', 'bashgetopt.c']
        # lib/sh's exact list, from bash's own build rather than a glob. 22 of
        # its 75 sources are REPLACEMENTS configure compiles only where the
        # system lacks the function -- getcwd, strerror, times, strtoul and so
        # on -- and compiling those here would not merely be redundant, it
        # would define the very names kernel/native_libc.h rewrites and shadow
        # the routed versions with ones that talk to the host.
        bash_lib_sh = ['casemod', 'clktck', 'clock', 'eaccess', 'fmtullong',
            'fmtulong', 'fmtumax', 'fnxform', 'fpurge', 'getenv',
            'gettimeofday', 'input_avail', 'itos', 'mailstat', 'makepath',
            'mbscasecmp', 'mbschr', 'mbscmp', 'mktime', 'netconn', 'netopen',
            'oslib', 'pathcanon', 'pathphys', 'random', 'setlinebuf', 'shmatch',
            'shmbchar', 'shquote', 'shtty', 'snprintf', 'spell', 'stringlist',
            'stringvec', 'strnlen', 'strtoimax', 'strtrans', 'strvis', 'timers',
            'timeval', 'tmpfile', 'uconvert', 'ufuncs', 'unicode', 'utf8',
            'wcsdup', 'wcsnwidth', 'winsize', 'zcatfd', 'zgetline', 'zmapfd',
            'zread', 'zwrite']
        bash_lib_other = ['lib/glob/glob.c', 'lib/glob/gmisc.c',
            'lib/glob/smatch.c', 'lib/glob/strmatch.c',
            'lib/glob/xmbsrtowcs.c', 'lib/tilde/tilde.c',
            # readline in full. It was left out of the first cut as "a large
            # body of code doing its own terminal handling", and that turned
            # out to cost the two things a shell is judged by: `shopt -s
            # progcomp` fails without it, because config-bot.h undefines
            # PROGRAMMABLE_COMPLETION when READLINE is off, and \[ \] in PS1
            # print LITERALLY, because their handling in decode_prompt_string
            # sits inside #if defined (READLINE). A prompt full of \[\] and no
            # line editing is not a shell anyone would set as their default.
            'lib/readline/bind.c', 'lib/readline/callback.c',
            'lib/readline/colors.c', 'lib/readline/compat.c',
            'lib/readline/complete.c', 'lib/readline/display.c',
            'lib/readline/funmap.c', 'lib/readline/histexpand.c',
            'lib/readline/histfile.c', 'lib/readline/history.c',
            'lib/readline/histsearch.c', 'lib/readline/input.c',
            'lib/readline/isearch.c', 'lib/readline/keymaps.c',
            'lib/readline/kill.c', 'lib/readline/macro.c',
            'lib/readline/mbutil.c', 'lib/readline/misc.c',
            'lib/readline/nls.c', 'lib/readline/parens.c',
            'lib/readline/parse-colors.c', 'lib/readline/readline.c',
            'lib/readline/rltty.c', 'lib/readline/savestring.c',
            'lib/readline/search.c', 'lib/readline/shell.c',
            'lib/readline/signals.c', 'lib/readline/terminal.c',
            'lib/readline/text.c', 'lib/readline/tilde.c',
            'lib/readline/undo.c', 'lib/readline/util.c',
            'lib/readline/vi_mode.c', 'lib/readline/xfree.c',
            'lib/readline/xmalloc.c']
        # NOT lib/termcap/termcap.c and lib/termcap/tparam.c, which readline
        # used to get tgetent and friends from. Two reasons to drop the files
        # rather than keep compiling them with NATIVE_LIBC_OWN_TERMCAP:
        #
        #   * They resolve nothing. That termcap reads /etc/termcap only, and
        #     the rootfs images ship terminfo instead. Leaving a second, dead
        #     implementation in the binary under the standard names is a trap
        #     for the next unrouted call site, not a fallback.
        #   * Nothing outside them uses them. readline (terminal.c, display.c)
        #     is the only caller of the six functions, tparam has no callers at
        #     all, and bashline.c only DECLARED tputs.
        #
        # What went with them: PC, which lib/readline/terminal.c now defines
        # itself -- it is the only code that writes it -- and ospeed, which
        # nothing in bash references. deps/bash-shim/termcap.h holds the
        # declarations, and lib/termcap is off the include path above so it is
        # the only <termcap.h> bash can find.

        bash_src = []
        foreach f : bash_top
            bash_src += files(bash_dir / f)
        endforeach
        foreach f : bash_builtins_static
            bash_src += files(bash_dir / 'builtins' / f)
        endforeach
        foreach f : bash_lib_sh
            bash_src += files(bash_dir / 'lib/sh' / (f + '.c'))
        endforeach
        foreach f : bash_lib_other
            bash_src += files(bash_dir / f)
        endforeach
        bash_src += files(bash_dir / 'generated/syntax.c')

        # kernel/native.c declares native_bash_main weak, so the registry entry
        # resolves only when this file is in the build.
        src += files('kernel/bash_glue.c')

        libbash = static_library('bash',
            bash_src + bash_builtin_src + [bash_builtins_tbl],
            include_directories: [includes, bash_includes],
            c_args: bash_defs)
    endif

    # ------------------------------------------------------------------- zsh
    #
    # zsh as a third native program, EXPERIMENTAL and default-off (see
    # meson_options.txt). Unlike bash this is not a submodule: deps/zsh is a
    # zsh source tree that has been CONFIGURED AND BUILT in place by
    # tools/configure-zsh.sh, and this target compiles the result. zsh
    # generates a lot of its own source -- config.h, Src/signames.c,
    # Src/bltinmods.list, and for every module a .mdh, an .epro and a .pro --
    # through mkmakemod.sh, makepro.awk and mkbltnmlst.sh, and reproducing
    # that machinery in meson would be a second build system to keep honest.
    # Compiling zsh's own generated files is the smaller commitment; the price
    # is that deps/zsh must be configured before this build runs, which is
    # what the check at the top of this file enforces.
    #
    # The generated headers include each other by RELATIVE path -- a module's
    # .mdh does `#include "../../Src/zsh.mdh"` and that does `#include
    # "../config.h"` -- so the tree layout is load-bearing and the include
    # list below is smaller than zsh's own Makefile passes.
    libzsh = []
    if have_zsh
        zsh_dir = 'deps/zsh'
        zsh_includes = include_directories(
            # FIRST, so <termcap.h> resolves to deps/zsh-shim/termcap.h on both
            # platforms. The iOS SDK ships libtermcap.tbd and libncurses.tbd but
            # no termcap.h, and zsh brings no termcap of its own -- so the
            # symbols link and only the declarations were missing. See that
            # header for why macOS uses it too rather than the system one.
            'deps/zsh-shim',
            zsh_dir, zsh_dir / 'Src', zsh_dir / 'Src/Zle',
            zsh_dir / 'Src/Modules', zsh_dir / 'Src/Builtins')

        # The seven names zsh and libish both define, out of zsh's 1941
        # externals against libish's 5216. Renaming ZSH's side keeps each
        # rename inside one archive, which is the rule the OpenSSH block above
        # follows for the same reason.
        #
        # Three come from bash (evalstring.c, evalfile.c, lib/sh/zgetline.c)
        # and four from OpenSSH (scp.c's `pwd` and `source`, and
        # openbsd-compat/bsd-setres_id.c). The setres* pair is the one worth a
        # word: zsh vendors OpenSSH's file too (Src/openssh_bsd_setres_id.c),
        # so both archives carry an implementation. They are NOT
        # interchangeable -- each is compiled with kernel/native_libc.h inside
        # its own archive, and the guest-vs-host question is settled by which
        # copy the call site reaches. Renaming keeps zsh's calls in zsh's copy.
        zsh_renames = [
            '-Dparse_string=zsh_parse_string',    # bash builtins/evalstring.c
            '-Dsourcelevel=zsh_sourcelevel',      # bash builtins/evalfile.c
            '-Dzgetline=zsh_zgetline',            # bash lib/sh/zgetline.c
            '-Dpwd=zsh_pwd',                      # OpenSSH scp.c
            '-Dsource=zsh_source',                # OpenSSH scp.c
            '-Dsetresuid=zsh_setresuid',          # OpenSSH bsd-setres_id.c
            '-Dsetresgid=zsh_setresgid',          # OpenSSH bsd-setres_id.c
        ]

        zsh_defs = [
            '-DHAVE_CONFIG_H',
            # The capability-code tables exist (kernel/native_termcap.c
            # exports them under ncurses's names, declared in the shim's
            # termcap.h), so zsh/termcap enumerates $termcap from THOSE rather
            # than from its own bundled ncurses-5-era fallback. Not cosmetic:
            # with the fallback, ${(k)termcap} listed 37 capabilities where
            # the guest's own zsh listed 50, for the same terminal.
            #
            # deps/zsh/config.h leaves these undefined because zsh's configure
            # ran without a curses library to find them in. They belong on the
            # command line rather than in that file: this is AOK supplying
            # them, and deps/zsh is a vendored tree.
            '-DHAVE_BOOLCODES', '-DHAVE_NUMCODES', '-DHAVE_STRCODES',
            # zsh brings its own globbing (Src/glob.c defines zglob and
            # friends) and never calls libc's. Same escape hatch bash uses --
            # see kernel/native_libc.h -- and here it is also what keeps
            # macOS's <glob.h> from being dragged in at all.
            '-DNATIVE_LIBC_OWN_GLOB',
            '-include', meson.current_source_dir() / 'kernel/native_libc.h',
            # AFTER the shim, deliberately: it exists to take back the handful
            # of rewrites zsh cannot live with. See the header for the rule.
            '-include', meson.current_source_dir() / 'deps/zsh-shim/aok_zsh_shim.h',
            # Same reason bash and smallclue carry it: zsh calls wcwidth, which
            # glibc puts behind a feature-test macro and Darwin exposes anyway.
            '-D_GNU_SOURCE',
            '-w',  # vendored code, not ours to police
        ] + zsh_renames

        # From zsh's own build (Src/stamp-modobjs), not a glob: several .c in
        # the tree belong to modules this build maps to link=no in
        # config.modules, and Src/main.c is a three-line wrapper around
        # zsh_main that would define a second main().
        zsh_top = [
            'builtin.c', 'compat.c', 'cond.c', 'context.c', 'exec.c', 'glob.c',
            'hashtable.c', 'hashnameddir.c', 'hist.c', 'init.c', 'input.c',
            'jobs.c', 'lex.c', 'linklist.c', 'loop.c', 'math.c', 'mem.c',
            'module.c', 'options.c', 'params.c', 'parse.c', 'pattern.c',
            'prompt.c', 'signals.c', 'signames.c', 'sort.c', 'string.c',
            'subst.c', 'text.c', 'utils.c', 'openssh_bsd_setres_id.c',
            # AOK's own, not upstream zsh's: fork by re-launch. It lives in the
            # zsh tree beside the code it patches, exactly as deps/bash's
            # aok_fork.c does, because only zsh can serialise zsh's state.
            'aok_fork.c']
        zsh_builtins = ['rlimits.c', 'sched.c']
        zsh_modules = [
            'attr.c', 'clone.c', 'datetime.c', 'files.c', 'hlgroup.c',
            'ksh93.c', 'langinfo.c', 'mapfile.c', 'mathfunc.c', 'nearcolor.c',
            'newuser.c', 'param_private.c', 'parameter.c', 'random.c',
            'random_real.c', 'regex.c', 'socket.c', 'stat.c', 'system.c',
            'errnames.c', 'tcp.c', 'termcap.c', 'watch.c', 'zftp.c', 'zprof.c',
            'zpty.c', 'zselect.c', 'zutil.c']
        zsh_zle = [
            'compctl.c', 'complete.c', 'compcore.c', 'compmatch.c',
            'compresult.c', 'complist.c', 'computil.c', 'deltochar.c',
            'zle_bindings.c', 'zle_hist.c', 'zle_keymap.c', 'zle_main.c',
            'zle_misc.c', 'zle_move.c', 'zle_params.c', 'zle_refresh.c',
            'zle_thingy.c', 'zle_tricky.c', 'zle_utils.c', 'zle_vi.c',
            'zle_word.c', 'termquery.c', 'textobjects.c', 'zleparameter.c']

        zsh_src = []
        foreach f : zsh_top
            zsh_src += files(zsh_dir / 'Src' / f)
        endforeach
        foreach f : zsh_builtins
            zsh_src += files(zsh_dir / 'Src/Builtins' / f)
        endforeach
        foreach f : zsh_modules
            zsh_src += files(zsh_dir / 'Src/Modules' / f)
        endforeach
        foreach f : zsh_zle
            zsh_src += files(zsh_dir / 'Src/Zle' / f)
        endforeach

        # kernel/native.c's registry entry is behind ISH_NATIVE_ZSH, so this
        # file and the define are decided by the same `have_zsh`.
        src += files('kernel/zsh_glue.c')

        # implicit_include_directories: false for the reason spelled out in
        # the OpenSSH block -- meson would otherwise put AOK's source root
        # ahead of everything here, and AOK's root has files whose names zsh
        # also uses.
        libzsh = static_library('zsh', zsh_src,
            include_directories: zsh_includes,
            implicit_include_directories: false,
            c_args: zsh_defs)
    endif

    # link_whole, not link_with: this folds SmallCLUE's objects into libish.a
    # itself. The Xcode project links the meson archives by name (libish.a,
    # libish_emu.a, libfakefs.a), so a fourth archive would silently fail to
    # link on iOS -- which is exactly what happened -- and would need adding to
    # every app target by hand. Riding inside libish.a keeps the two build
    # systems from diverging again.
    # -DAOK_HAVE_OPENSSH is read by kernel/openssh_glue.c alone, and is passed
    # here rather than decided in that file so that the archive and the glue
    # cannot disagree: one `have_openssh` decides both.
    ish_c_args = []
    if have_openssh
        ish_c_args += '-DAOK_HAVE_OPENSSH'
    endif
    # Same contract for the checksums: one `have_checksums` decides both
    # whether checksum_app.c is in the archive and whether smallclue_glue.c
    # defines the refusals, so the two cannot disagree. They must not both be
    # present -- the linker takes the first archive member defining a symbol
    # and stops, which is how `ssh -V` once printed "unavailable" out of a
    # binary containing all of OpenSSH.
    if have_checksums
        ish_c_args += '-DAOK_HAVE_CHECKSUMS'
    endif
    # One flag decides both the archive and the registry entry, so the two
    # cannot disagree -- the same contract as AOK_HAVE_OPENSSH above.
    if have_rust_native
        ish_c_args += '-DISH_NATIVE_RUST'
    endif
    # One flag decides both the object and the registry entry, so the two
    # cannot disagree -- the same contract as AOK_HAVE_OPENSSH.
    if have_helix
        ish_c_args += '-DISH_NATIVE_HELIX'
    endif

    libish = library('ish', src,
        objects: librustnative,
        link_whole: [libsmallclue, libnextvi, libbash, libzsh, libopenssh] + libopenssh_special + libcurlshim,
        include_directories: includes, c_args: ish_c_args)
    ish = declare_dependency(
        link_with: [libish, libish_emu, libfakefs],
        dependencies: dependencies,
        include_directories: includes)

    if not meson.is_cross_build()
        executable('ish', ['main.c'], dependencies: ish,
            c_args: ['-DISH_SOURCE_ROOT="' + meson.project_source_root() + '"'])
    endif

elif get_option('kernel') == 'linux'
    modules = static_library('linux_modules', [
        'linux/main.c',
        'linux/fakefs.c',
    ], dependencies: [linux_headers, sqlite3])
    executable('ish', [build_linux],
        link_with: [libfakefs],
        dependencies: [
            liblinux,
            libm,
            libdl,
            threads,
            declare_dependency(link_whole: modules),
        ])
endif

subdir('tools')

gdb_scripts = ['ish-gdb.gdb']
foreach script : gdb_scripts
    custom_target(script,
        output: script, input: script,
        command: ['ln', '-sf', '@INPUT@', '@OUTPUT@'],
        build_by_default: true)
endforeach

if not meson.is_cross_build()
    # test for floating point library
    float80_test = executable('float80_test', ['emu/float80.c', 'emu/float80-test.c'], dependencies: [libm])
    test('float80', float80_test)

    # riscv64 decode.h check: RVC expansion + immediate reassembly against
    # llvm-mc-derived vectors (tests/riscv64/decode_vectors.h, checked in).
    riscv64_decode_check = executable('riscv64_decode_check',
        ['tests/riscv64/decode_check.c'],
        include_directories: [includes, include_directories('tests/riscv64')])
    test('riscv64_decode', riscv64_decode_check)
endif

e2e_test = find_program('tests/e2e/e2e.bash')
# First run builds the test filesystem: it downloads an Alpine rootfs and runs
# `apk add build-base ...` under emulation, then the qemu test compiles a large
# C file with guest gcc. 180s was nowhere near enough on CI runners.
test('e2e', e2e_test, args: ['-y'], timeout: 1800)
