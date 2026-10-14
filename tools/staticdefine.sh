#!/bin/bash -e
compile_commands=$1
input=$2
output=$3
dep=$4
tmp_output="$output.tmp"
tmp_dep="$dep.tmp"
flags=$(python3 - <<END
import json
with open('$compile_commands') as f:
    commands = json.load(f)
for command in commands:
    if command['file'].endswith('jit/jit.c'):
        break
command = command['command']
command = command.split()[:-9] + ['-MD', '-MQ', '$output', '-MF', '$tmp_dep']
print(' '.join(command))
END
)
$flags $input -include "$(dirname $0)/staticdefine.h" -S -o - | \
sed -ne 's:^[[:space:]]*\.ascii[[:space:]]*"\(.*\)".*:\1:;
         /^->/{s:->#\(.*\):/* \1 */:;
         s:^->\([^ ]*\) [\$$#]*\([^ ]*\) \(.*\):#define \1 \2 /* \3 */:;
         s:->::; p;}' > "$tmp_output"
# sed magic was copied from the linux kernel build system

changed=1
if [ -f "$output" ] && cmp -s "$tmp_output" "$output"; then
    changed=0
    rm -f "$tmp_output"
else
    mv "$tmp_output" "$output"
fi
mv "$tmp_dep" "$dep"

if [ "$changed" -ne 0 ]; then
    builddir=$(cd "$(dirname "$compile_commands")" && pwd)
    rm -f "$builddir"/libish_emu.a.p/jit_gadgets-*.S.o "$builddir"/libish_emu_math.a.p/jit_gadgets-*.S.o
fi
