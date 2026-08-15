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
have_bash = fs_early.exists('deps/bash/shell.c')
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
dependencies = [librt, libm, libdl, libatomic, threads, sqlite3]

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
    ['fs/fake-db.c', 'fs/fake-migrate.c', 'fs/fake-rebuild.c'],
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
        'kernel/native_syscall.c', native_syscall_nums,
        'kernel/smallclue_glue.c',
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
    # checksum_app needs OpenSSL (AOK links none), integration.c is the
    # WITH_EXSH path into PSCAL's VM, and the openssh trio needs the vendored
    # OpenSSH tree.
    # gzip_app and tar_app are excluded for a build-system reason rather than a
    # code one: they are the only files needing zlib, and the Xcode app targets
    # link the meson archives by name with no zlib anywhere in the project (no
    # libz.tbd, no OTHER_LDFLAGS). Re-enabling either means adding zlib to
    # those targets first -- worth doing, since tar is a real loss, but it is a
    # change to the app project rather than to this build.
    smallclue_excluded = ['main.c', 'integration.c', 'checksum_app.c',
                          'openssh_app.c', 'openssh_shim.c', 'openssh_stubs.c',
                          'gzip_app.c', 'tar_app.c']

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
        'deps/smallclue',
        'deps/smallclue/src',
        'deps/smallclue/common',
        'deps/smallclue/compat',
    )

    smallclue_c_args = [
        '-include', meson.current_source_dir() / 'kernel/native_libc.h',
        '-DSMALLCLUE_PLATFORM_SPAWN',
        '-w',  # vendored code, not ours to police
    ]

    libsmallclue = static_library('smallclue', smallclue_src,
        include_directories: [includes, smallclue_includes],
        c_args: smallclue_c_args)

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
            'deps/bash-shim',   # <builtins/builtext.h>; see the header there
            bash_dir, bash_dir / 'generated', bash_dir / 'generated/builtins',
            bash_dir / 'include', bash_dir / 'builtins', bash_dir / 'lib',
            bash_dir / 'lib/readline', bash_dir / 'lib/termcap',
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
            # bash brings its own globbing, and macOS's <glob.h> shares the
            # include guard _GLOB_H_ with bash's -- see kernel/native_libc.h.
            '-DNATIVE_LIBC_OWN_GLOB',
            # fork by re-launch (deps/bash/aok_fork.c). Guards the two hooks in
            # bash's own sources so the vendor tree still builds without it.
            '-DAOK_NATIVE_FORK=1',
            # The two symbols that collide with libish, out of 1837 against
            # 1775. main is the obvious one; wait_for is bash's job-control
            # reaper against AOK's condition-variable wait (util/sync.h).
            '-Dmain=bash_main_entry',
            '-Dwait_for=bash_wait_for',
            '-include', meson.current_source_dir() / 'kernel/native_libc.h',
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
            'lib/readline/xmalloc.c',
            # bash's OWN termcap, not the system one. readline needs tgetent
            # and friends, and bash's configure links -ltermcap when it finds
            # it -- which would put a system library on the Xcode link line,
            # the very thing that keeps zlib (and tar, and gzip) out. The
            # bundled implementation costs three files and no dependency.
            'lib/termcap/termcap.c', 'lib/termcap/tparam.c']

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

    # link_whole, not link_with: this folds SmallCLUE's objects into libish.a
    # itself. The Xcode project links the meson archives by name (libish.a,
    # libish_emu.a, libfakefs.a), so a fourth archive would silently fail to
    # link on iOS -- which is exactly what happened -- and would need adding to
    # every app target by hand. Riding inside libish.a keeps the two build
    # systems from diverging again.
    libish = library('ish', src,
        link_whole: [libsmallclue, libnextvi, libbash],
        include_directories: includes)
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
