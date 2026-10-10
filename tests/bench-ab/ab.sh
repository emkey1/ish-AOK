#!/bin/bash
# ab.sh <name> <udid> <rounds> <ssh args...> -- <arch:root>...
#
# Alternates two app builds on one device -- $A then $B, <rounds> times -- and
# runs bench.sh in each, so slow drift (heat, background work) lands on both
# builds alike. The apps are $SP/app-$A/iSH-AOK.app and $SP/app-$B/iSH-AOK.app
# (SP defaults to this script's directory; A and B to 557 and 558). Installs are
# serialised across concurrent ab.sh runs by $SP/install.lock: one hung
# `devicectl device install` can wedge CoreDevice for every device. Each launch
# skips the restore-sessions prompt. Results land in the device's
# ~/bench558/results.txt and in $SP/ab-<name>.log; summarise.py reads either.
#
# Device setup, once: bench.sh at ~/bench558/bench.sh, pybench.py at
# /AOK/fakefs/bench/pybench.py (bound into every chroot), 7z and python3 in
# each root named, and passwordless sudo for mount-root.sh.
SP=${SP:-$(cd "$(dirname "$0")" && pwd)}
A=${A:-557} B=${B:-558}
name=$1 udid=$2 rounds=$3; shift 3
ssh_args=(); while [ "$1" != "--" ]; do ssh_args+=("$1"); shift; done; shift
specs=("$@")
log=$SP/ab-$name.log
s() { ssh -o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=30 -o ServerAliveCountMax=4 "${ssh_args[@]}" "$@"; }
for r in $(seq 1 $rounds); do
  for b in $A $B; do
    # one install at a time across devices
    until mkdir $SP/install.lock 2>/dev/null; do sleep 5; done
    echo "$(date +%T) round $r build $b: install" >> $log
    xcrun devicectl device install app --device $udid $SP/app-$b/iSH-AOK.app >> $log 2>&1
    rmdir $SP/install.lock
    xcrun devicectl device process launch --device $udid --terminate-existing --environment-variables '{"ISH_SESSION_RESUME":"fresh"}' app.ish.iSH-AOK >> $log 2>&1
    n=0; until s true 2>/dev/null || [ $n -ge 60 ]; do sleep 10; n=$((n+1)); done
    sleep 20   # let the boot's own work settle
    echo "$(date +%T) round $r build $b: $(s 'dmesg | grep -m1 -o "built [A-Z][a-z]* [0-9]* [0-9]* [0-9:]*"')" >> $log
    s "sh ~/bench558/bench.sh r$r-b$b ${specs[*]} > /dev/null 2>&1; tail -$(( ${#specs[@]} + 1 )) ~/bench558/results.txt" >> $log 2>&1
  done
done
echo "ALLDONE $(date +%T)" >> $log
