#!/bin/sh
# Can this clang build every vDSO -- vdso.S for i386, and amd64/, arm64/ and
# riscv64/ -- each an ELF shared object linked by lld?
cc=$1
test_c=$(mktemp)
cat > $test_c <<END
#if !defined(__ELF__)
#error "__ELF__ is not defined"
#endif
END
status=0
for target in i386-linux x86_64-linux aarch64-linux riscv64-linux; do
    cmd="$cc -target $target -fuse-ld=lld -shared -nostdlib -x c $test_c -o /dev/null"
    echo $ $cmd
    $cmd || status=$?
done
rm -f $test_c
exit $status
