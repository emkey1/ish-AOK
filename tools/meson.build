if get_option('kernel') == 'ish'
    # these tools are specific to x86_64 linux
    if host_machine.system() == 'linux' and host_machine.cpu() == 'x86_64'
        transplant_src = [
            'vdso-transplant.c',
            'ptutil.c',
        ]
        executable('ptraceomatic', ['ptraceomatic.c', 'undefined-flags.c', transplant_src], dependencies: [ish])
        configure_file(input: 'ptraceomatic-gdb.gdb', output: '@PLAINNAME@', copy: true)

        # tools for messing with vdsos
        # executable('vdso-dump', ['vdso-dump.c'], c_args: ['-m32'], link_args: ['-m32'])
        executable('vdso-transplant', ['vdso-transplant-main.c', transplant_src],
            include_directories: includes)
    endif

    unicorn = disabler()
    if host_machine.system() == 'darwin'
        unicorn_dirs = ['/usr/local/lib']
        if host_machine.cpu_family() == 'aarch64'
            unicorn_dirs = [
                '/opt/homebrew/opt/unicorn/lib',
                '/opt/homebrew/lib',
            ]
        endif
        unicorn = cc.find_library('unicorn', dirs: unicorn_dirs, required: false)
    else
        unicorn = cc.find_library('unicorn', required: false)
    endif
    if unicorn.found()
        executable('unicornomatic', ['unicornomatic.c', 'undefined-flags.c'], dependencies: [ish, unicorn])
        configure_file(input: 'ptraceomatic-gdb.gdb', output: 'unicornomatic-gdb.gdb', copy: true)
    endif
endif

libarchive = dependency('libarchive', required: false)
if not libarchive.found()
    # Homebrew, which is keg-only for libarchive (macOS ships its own), so it is
    # not on the default pkg-config path and has to be found by prefix.
    #
    # Search the prefix for THIS machine's architecture first. A Mac that has
    # ever run the Intel Homebrew keeps it at /usr/local alongside the Apple
    # Silicon one at /opt/homebrew, and trying /usr/local first picked the
    # x86_64 build on an arm64 host: find_library only looks for the file, so
    # configure succeeded and the failure landed at link time as a wall of
    # "Undefined symbols for architecture arm64" behind one easily-missed
    # "ignoring file ... found architecture 'x86_64'". Seen on an M5 with a
    # 2021 /usr/local install still present.
    if host_machine.cpu_family() == 'aarch64'
        libarchive_prefixes = ['/opt/homebrew/opt/libarchive', '/usr/local/opt/libarchive']
    else
        libarchive_prefixes = ['/usr/local/opt/libarchive', '/opt/homebrew/opt/libarchive']
    endif
    foreach prefix : libarchive_prefixes
        if not libarchive.found()
            libarchive_lib = cc.find_library('archive', dirs: [prefix + '/lib'], required: false)
            # find_library does not check the architecture, so confirm the
            # library actually links before settling on it. Without this the
            # ordering above is only a guess about which prefix is usable.
            if libarchive_lib.found() and cc.links(
                    '#include <archive.h>\nint main(void) { return archive_read_new() == 0; }',
                    dependencies: [libarchive_lib],
                    include_directories: include_directories([prefix + '/include']),
                    name: 'libarchive in ' + prefix)
                libarchive = declare_dependency(dependencies: [libarchive_lib], include_directories: include_directories([prefix + '/include']))
            endif
        endif
    endforeach
endif

if libarchive.found()
    fakefsify = executable('fakefsify', ['fakefsify.c', 'fakefs.c'], dependencies: [libarchive], link_with: [libfakefs], include_directories: [includes])
    custom_target('unfakefsify',
        build_by_default: true,
        command: ['ln', '-sf', 'fakefsify', '@OUTPUT@'],
        output: 'unfakefsify')
endif
