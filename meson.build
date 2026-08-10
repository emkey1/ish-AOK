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

    libish = library('ish', src,
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
