# The VDSO gets inserted into the address space of emulated processes, so it
# needs to be compiled as an i386 ELF shared library. This requires a cross
# compiler and linker on most of the platforms that matter (Mac, 64-bit Linux,
# Raspberry Pi). Clang and LLD are easy to install on all of these platforms,
# so that's currently the only supported option (though if you have some other
# compatible toolchain you can edit this file to point to it instead.)

# Default install paths for Homebrew and MacPorts. The prefix for THIS machine's
# architecture comes first: /opt/homebrew is Apple silicon's, /usr/local is the
# Intel one, and a Mac that has ever run the Intel Homebrew keeps both. Trying
# /usr/local first meant an arm64 Mac built the VDSO with whatever ancient
# x86_64 clang was left in the old prefix, under Rosetta -- on an M5 here that
# was a clang-12 from 2021, silently preferred over the native clang 22. It
# produced a correct VDSO (this is a cross-compile to i386-linux either way, so
# the host arch of the compiler does not change the output) which is why it went
# unnoticed, but it was slower, four years stale, and it quietly made a
# removable leftover into a build dependency.
#
# check-cc.sh below still has the final say: it tests that the chosen compiler
# can actually produce an i386 ELF with lld, so a wrong guess here fails loudly
# rather than silently.
if host_machine.cpu_family() == 'aarch64'
    clang_candidates = ['/opt/homebrew/opt/llvm/bin/clang', '/usr/local/opt/llvm/bin/clang']
else
    clang_candidates = ['/usr/local/opt/llvm/bin/clang', '/opt/homebrew/opt/llvm/bin/clang']
endif
clang = find_program(clang_candidates + ['/opt/local/bin/clang', 'clang'])
check_cc = find_program('check-cc.sh')
result = run_command(check_cc, clang, check: false)
if result.returncode() != 0
    message('\n' + result.stdout() + result.stderr())
    if build_machine.system() == 'darwin'
        install_msg = 'Install LLVM through Homebrew (brew install llvm)'
    elif build_machine.system() == 'linux'
        install_msg = 'Install Clang and LLD using your package manager (e.g. sudo apt install clang lld)'
    endif
    error('Could not find usable VDSO compiler. ' + install_msg)
endif
vdso_compiler = [clang, '-target', 'i386-linux', '-fuse-ld=lld']

vdso = custom_target('vdso', input: ['vdso.S', 'vdso.c', 'vdso.lds'], output: 'libvdso.so.elf',
    command: vdso_compiler + ['-o', '@OUTPUT@', '@INPUT0@', '@INPUT1@',
        '-nostdlib', '-Wl,-T,@INPUT2@', '-Wl,--hash-style,sysv', '-shared', '-fPIC']
        + get_option('vdso_c_args').split())
