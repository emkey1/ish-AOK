#!/bin/sh
# vm-update.sh -- AOK VM's aok-update lives in its own repository since 2026-10-14
# (emkey1/aok-vm, aok/machine/aok-update), with the /AOK/tools scripts its machines carry. A machine
# made before that runs this, as its aok-update brings it: this fetches that one and runs it, and it
# replaces /usr/local/bin/aok-update with itself.
set -e
u=${AOK_UPDATE_URL:-https://raw.githubusercontent.com/emkey1/aok-vm/aok-vm}/aok/machine/aok-update
t=$(mktemp)
trap 'rm -f "$t"' EXIT
if ! wget -q -O "$t" "$u" || ! [ -s "$t" ] || ! sh -n "$t" 2> /dev/null; then
    echo "aok-update: could not fetch a sound aok-update from $u; nothing was changed"
    exit 1
fi
AOK_UPDATE_AGAIN=1 sh "$t" "$@"
