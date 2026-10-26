#!/bin/sh
# Run the guest regression suite inside the iOS APP, in simulators on another
# Mac -- the stand-in for a device leg when no device is at hand.
#
#     tools/sim-suite.sh run    <host> <app> [root...]   one simulator per root, return at once
#     tools/sim-suite.sh status <host> [label]           pass/fail per root
#     tools/sim-suite.sh clean  <host>                   delete the suite's simulators
#
# e.g. tools/sim-suite.sh run MBP5.local "$(xcodebuild -project iSH-AOK.xcodeproj \
#          -scheme iSH-AOK -configuration Debug-ApplePleaseFixFB19282108 \
#          -sdk iphonesimulator -showBuildSettings | sed -n 's/^ *CODESIGNING_FOLDER_PATH = //p')"
#
# <app> is a simulator build of the app made from this checkout's HEAD
# (xcodebuild -sdk iphonesimulator -destination 'generic/platform=iOS
# Simulator'). Every worktree has its own DerivedData, so ask xcodebuild for
# the path rather than globbing. It is copied to the host as
# ~/ish-sim-suite/<label>/iSH-AOK.app, where <label> is the build number and
# HEAD's short SHA: built HERE because the host may not be able to (a
# Homebrew Rust has no iOS simulator target), compressed and resumable
# because the link may be slow -- a run from away reached the MBP over a
# Tailscale relay at 30-200 KB/s. A second run at the same label sends only
# what changed.
#
# What a leg does, on the host, one per root and all at once:
#   - makes a fresh simulator, "AOK sim suite <root>" (a reused one would carry
#     the last run's app data), boots it and installs the app;
#   - copies the host's build/<root> -- the roots tools/remote-suite.sh runs
#     on -- into the app's App Group as roots/<root>, with cp -c, so the copy
#     is a clone and the original is never booted;
#   - sets the terminal's launch command (the "Init Command" default) to run
#     setup-regressions.sh --run into the root's /root/sim-suite.log, and
#     launches the app with ISH_BOOT_ROOT=<root>, so the root boots as the
#     running root under its own init, as a device leg does since 557;
#   - reads that log from the HOST side, under the App Group -- nothing needs
#     ssh into the guest -- copying it to ~/ish-sim-suite/<label>/<root>.log
#     until it ends with SUITE-EXIT, then shuts the simulator down.
#
# The simulator is the app, not a device: the same app code, kernel and JIT,
# on the Mac's CPU and without iOS's memory limits, thermal state or real
# suspension. It catches what the CLI cannot see of the app and nothing about
# the hardware. Five legs at once load the host the way remote-suite's do, so
# timer, rusage and inotify tests can fail under them and pass alone.
#
# SIM_DEVICE and SIM_RUNTIME pick the simulator (an M5 iPad Pro and the
# host's newest iOS runtime by default).
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
    [ $# -ge 1 ] || usage
    app=${1%/}
    shift
    roots=${*:-$ROOTS_DEFAULT}
    [ -f "$app/Info.plist" ] || { echo "sim-suite: $app is not an app bundle" >&2; exit 1; }
    build=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$app/Info.plist")
    label=$build-$(git rev-parse --short=9 HEAD)
    remote "mkdir -p ~/ish-sim-suite/$label"
    rsync -az --partial --delete "$app/" "$host:ish-sim-suite/$label/iSH-AOK.app/"

    # One leg: a simulator, a root, its log. Installed on the host each run,
    # by RENAME: legs already running are bash reading this file by offset,
    # and rewriting it in place under them (a rerun of one root while the
    # others run) would hand them the middle of the new text.
    ssh -o BatchMode=yes "$host" 'cat > ~/ish-sim-suite/.sim-leg.sh.new && mv ~/ish-sim-suite/.sim-leg.sh.new ~/ish-sim-suite/sim-leg.sh' <<'LEG'
#!/bin/bash
# sim-leg.sh <app> <root> <dir> <device type> <runtime>, run from the checkout
app=$1 root=$2 dir=$3 devtype=$4 runtime=$5
name="AOK sim suite $root"
note() { echo "$(date '+%F %T') $*" >> "$dir/$root.leg"; }
for u in $(xcrun simctl list devices -j | python3 -I -c '
import json, sys
for devs in json.load(sys.stdin)["devices"].values():
    for d in devs:
        if d["name"] == sys.argv[1]:
            print(d["udid"])' "$name"); do
    xcrun simctl shutdown "$u" >/dev/null 2>&1 || true
    xcrun simctl delete "$u"
done
udid=$(xcrun simctl create "$name" "$devtype" "$runtime")
note "simulator $udid ($devtype, $runtime)"
xcrun simctl boot "$udid"
xcrun simctl bootstatus "$udid" -b > /dev/null
xcrun simctl install "$udid" "$app"
group=$(xcrun simctl get_app_container "$udid" app.ish.iSH-AOK groups |
        awk -F'\t' '$1 == "group.app.ish.iSH-AOK" { print $2 }')
[ -n "$group" ] || { note "no App Group container"; exit 1; }
mkdir -p "$group/roots"
cp -cR "build/$root" "$group/roots/$root"
# The launch command, which the terminal runs as root once the root boots.
#  - PATH is exported. The app starts it with none (Linux starts init with
#    none too; login sets one), and dash's own default is not exported, so
#    gcc found no PATH, could not locate itself, and every compile failed
#    with "cannot execute 'cc1'". This is the CLI's PATH.
#  - The test cache is copied out of /tmp first thing, and the suite pointed
#    at the copy. The root's cache is its /tmp/ish-aok-regress-cache (the
#    CLI suite's), and the app's own, /AOK/fakefs/regress-cache, is empty in
#    a fresh simulator.
#  - It then waits until no rc script is running (two minutes at most): the
#    terminal starts while init is still running the root's boot scripts,
#    and Devuan's clean /tmp, where the suite puts its binaries -- started at
#    once, 392 of its tests were "not found". A device leg, started over ssh
#    long after boot, always waited. The pattern is init[.]d/rc so that this
#    command line, itself a process's cmdline, does not match it.
# No double quotes inside: the command is a string in an old-style plist.
xcrun simctl spawn "$udid" defaults write app.ish.iSH-AOK "Init Command" \
    '("/bin/sh", "-c", "cd /root; { echo start; date; grep ^booted /proc/ish/roots; id; export PATH=/AOK/persist/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; [ -d /tmp/ish-aok-regress-cache ] && cp -a /tmp/ish-aok-regress-cache /root/.sim-regress-cache; export ISH_AOK_REGRESS_CACHE=/root/.sim-regress-cache; sleep 10; i=0; while [ $i -lt 60 ] && grep -aqs init[.]d/rc /proc/[0-9]*/cmdline; do sleep 2; i=$((i+1)); done; echo boot scripts done; date; sh /AOK/tests/setup-regressions.sh --run; echo SUITE-EXIT $?; } > /root/sim-suite.log 2>&1 < /dev/null; exec /bin/sh")'
SIMCTL_CHILD_ISH_BOOT_ROOT=$root SIMCTL_CHILD_ISH_SESSION_RESUME=fresh \
    xcrun simctl launch "$udid" app.ish.iSH-AOK >> "$dir/$root.leg" 2>&1
note "launched"
guest="$group/roots/$root/data/root/sim-suite.log"
deadline=$(( $(date +%s) + 4 * 3600 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    [ -f "$guest" ] && cp "$guest" "$dir/$root.log"
    grep -q '^SUITE-EXIT' "$dir/$root.log" 2>/dev/null && break
    sleep 30
done
grep -q '^SUITE-EXIT' "$dir/$root.log" 2>/dev/null || note "no SUITE-EXIT after 4 hours"
note "done"
xcrun simctl terminate "$udid" app.ish.iSH-AOK > /dev/null 2>&1 || true
xcrun simctl shutdown "$udid" > /dev/null 2>&1 || true
LEG

    devtype=${SIM_DEVICE:-com.apple.CoreSimulator.SimDeviceType.iPad-Pro-13-inch-M5-12GB}
    runtime=${SIM_RUNTIME:-}
    if [ -z "$runtime" ]; then
        runtime=$(ssh -o BatchMode=yes "$host" 'xcrun simctl list runtimes -j' | python3 -I -c '
import json, sys
ios = [r for r in json.load(sys.stdin)["runtimes"] if r["platform"] == "iOS" and r["isAvailable"]]
print(max(ios, key=lambda r: [int(x) for x in r["version"].split(".")])["identifier"])')
    fi
    remote "
        cd ~/git/ish-AOK
        dir=\$HOME/ish-sim-suite/$label
        devtype='$devtype'
        runtime='$runtime'
        for r in $roots; do
            if [ ! -d build/\$r ]; then echo \"sim-suite: no build/\$r on $host, skipped\" >&2; continue; fi
            rm -f \$dir/\$r.log \$dir/\$r.leg
            nohup bash ~/ish-sim-suite/sim-leg.sh \$dir/iSH-AOK.app \$r \$dir \"\$devtype\" \"\$runtime\" > /dev/null 2>&1 < /dev/null &
            sleep 5
        done
        echo \"started $label on $host: \$dir ($roots) on \$devtype, \$runtime\"
    "
    ;;
status)
    label=${1:-}
    remote "
        setopt nullglob 2> /dev/null || true
        cd ~/ish-sim-suite
        d=$label; [ -n \"\$d\" ] || d=\$(ls -t | grep -v sim-leg.sh | head -1)
        echo \"\$d on $host\"
        for f in \$d/*.leg; do
            r=\$(basename \$f .leg); l=\$d/\$r.log
            [ -f \$l ] || { printf '%-22s (no log yet) %s\n' \$r \"\$(tail -1 \$f)\"; continue; }
            printf '%-22s PASS %4d FAIL %3d SKIP %3d  %s\n' \$r \$(grep -c ': PASS' \$l) \$(grep -c ': FAIL' \$l) \$(grep -c ': SKIP' \$l) \"\$(tail -1 \$l | cut -c1-50)\"
        done
        grep -H '^booted' \$d/*.log /dev/null 2>/dev/null | sed 's|^[^/]*/||' || true
        grep -H ': FAIL' \$d/*.log /dev/null 2>/dev/null | sed 's|^[^/]*/||' || true
    "
    ;;
clean)
    remote "
        xcrun simctl list devices -j | python3 -I -c '
import json, sys
for devs in json.load(sys.stdin)[\"devices\"].values():
    for d in devs:
        if d[\"name\"].startswith(\"AOK sim suite \"):
            print(d[\"udid\"])' | while read u; do
            xcrun simctl shutdown \$u > /dev/null 2>&1 || true
            xcrun simctl delete \$u && echo \"deleted \$u\"
        done
    "
    ;;
*)
    usage
    ;;
esac
