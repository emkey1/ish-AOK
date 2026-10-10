#!/bin/sh
# Run the guest regression suite on another Mac, so this one only drives the
# devices.
#
#     tools/remote-suite.sh run    <host> [root...]   start the legs, return at once
#     tools/remote-suite.sh status <host> [sha]       pass/fail per root
#     tools/remote-suite.sh rerun  <host> <root> <test[,test...]>   one root, alone
#     tools/remote-suite.sh cache  <host>             merge the host's test cache here
#
# e.g. tools/remote-suite.sh run MBP5.local
#
# The host needs a checkout of this repo at ~/git/ish-AOK that can fetch
# origin, Homebrew's meson and ninja, a configured build/ (deps/dash prepared
# once: cd deps/dash && ../../tools/configure-dash.sh), and the roots under
# build/ -- rsync them from here, they are a few hundred MB each:
#     rsync -a build/alpine-arm64-324/ <host>:git/ish-AOK/build/alpine-arm64-324/
#
# `run` brings the host's checkout to origin/working (fast-forward only: it
# refuses a dirty tree or a diverged branch rather than touching either),
# builds ish, pins the binary as ~/ish-suite/<sha>/ish so a later rebuild
# cannot change a running leg, and starts one leg per root in parallel, each
# detached with nohup and logging to ~/ish-suite/<sha>/<root>.log. Push first:
# the host builds what origin has, and `run` prints the SHA it built.
#
# Parallel legs load the host, so timer, rusage and inotify tests can fail
# under them and pass alone; `rerun` runs the named tests on one root with
# nothing else from this script running.
#
# inaddr_any_iface fails on a host whose application firewall is on (System
# Settings > Network > Firewall): the firewall holds back inbound connections
# to a freshly built, unsigned ish on every interface but loopback, so the
# test reports "connected but no banner" for the LAN and Tailscale addresses.
# That is the host, not the guest. The MBP had it on when this was written
# (2026-10-10); this laptop has it off, where the test passes.
set -eu

ROOTS_DEFAULT="alpine-i386-test alpine-amd64-test alpine-arm64-324 alpine-riscv64-test devuan-arm64-test"

usage() {
    sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}
[ $# -ge 2 ] || usage
cmd=$1 host=$2
shift 2

remote() {
    ssh -o BatchMode=yes "$host" "export PATH=/opt/homebrew/bin:\$PATH; set -eu; $1"
}

case $cmd in
run)
    roots=${*:-$ROOTS_DEFAULT}
    # One leg: pinned ish, one root, its log. Installed on the host each run.
    ssh -o BatchMode=yes "$host" 'mkdir -p ~/ish-suite && cat > ~/ish-suite/leg.sh' <<'LEG'
#!/bin/sh
# leg.sh <pinned ish> <root> <log>, run from the checkout
ish=$1 root=$2 log=$3
{
    echo "start $(date) load $(sysctl -n vm.loadavg)"
    case $root in alpine-*) "$ish" -f "build/$root" /bin/sh -c 'apk add -q linux-headers >/dev/null 2>&1; true' ;; esac
    "$ish" -f "build/$root" /bin/sh -c 'sh /AOK/tests/setup-regressions.sh --run'
    echo "EXIT $? $(date)"
} > "$log" 2>&1 < /dev/null
LEG
    remote "
        cd ~/git/ish-AOK
        if [ -n \"\$(git status --porcelain --untracked-files=no)\" ]; then
            echo 'remote-suite: the checkout on $host has local changes; not touching it' >&2; exit 1
        fi
        git fetch -q origin
        git merge -q --ff-only origin/working
        git submodule update -q --init --recursive
        ninja -C build ish > /tmp/remote-suite-ninja.log 2>&1 || { tail -20 /tmp/remote-suite-ninja.log >&2; exit 1; }
        sha=\$(git rev-parse --short=9 HEAD)
        out=\$HOME/ish-suite/\$sha
        mkdir -p \$out
        cp build/ish \$out/ish
        for r in $roots; do
            if [ ! -d build/\$r ]; then echo \"remote-suite: no build/\$r on $host, skipped\" >&2; continue; fi
            nohup sh ~/ish-suite/leg.sh \$out/ish \$r \$out/\$r.log > /dev/null 2>&1 < /dev/null &
        done
        echo \"started \$sha on $host: \$out ($roots)\"
    "
    ;;
status)
    sha=${1:-}
    remote "
        cd ~/ish-suite
        d=$sha; [ -n \"\$d\" ] || d=\$(ls -t | grep -v leg.sh | head -1)
        echo \"\$d on $host\"
        for f in \$d/*.log; do
            r=\$(basename \$f .log)
            case \$r in rerun-*) continue ;; esac
            printf '%-22s PASS %4d FAIL %3d SKIP %3d  %s\n' \$r \$(grep -c ': PASS' \$f) \$(grep -c ': FAIL' \$f) \$(grep -c ': SKIP' \$f) \"\$(tail -1 \$f | cut -c1-50)\"
        done
        grep -H ': FAIL' \$d/*.log 2>/dev/null | sed 's|^[^/]*/||' || true
    "
    ;;
rerun)
    [ $# -ge 2 ] || usage
    root=$1 tests=$2
    remote "
        cd ~/git/ish-AOK
        d=~/ish-suite/\$(ls -t ~/ish-suite | grep -v leg.sh | head -1)
        echo \"\$d, $root, load \$(sysctl -n vm.loadavg)\"
        \$d/ish -f build/$root /bin/sh -c 'sh /AOK/tests/setup-regressions.sh --only $tests --run' < /dev/null 2>&1 | grep -E ': (PASS|FAIL|SKIP)'
    "
    ;;
cache)
    store=${ISH_AOK_REGRESS_STORE:-$HOME/.cache/ish-aok-regress-cache}
    mkdir -p "$store"
    before=$(ls "$store" | wc -l | tr -d ' ')
    # The suite on a Mac caches inside each root, at its /tmp.
    for r in $ROOTS_DEFAULT devuan-amd64-test; do
        rsync -a --ignore-existing "$host:git/ish-AOK/build/$r/data/tmp/ish-aok-regress-cache/" "$store/" 2>/dev/null || true
    done
    echo "cache: $(( $(ls "$store" | wc -l) - before )) new, $(ls "$store" | wc -l | tr -d ' ') in $store"
    ;;
*)
    usage
    ;;
esac
