#!/bin/sh
# native_mode.sh -- a native-mode root: no distribution, only /AOK/native.
#
# Run in a root made by kernel/native_root.c (docs/native_mode_plan.md). The
# gate's `native` leg (tools/run-guest-gate.sh) builds one from nothing each
# run and calls this in four phases, because some of what is checked is what a
# BOOT does, which a single process cannot watch happen to itself:
#
#   --setup-reprovision   break the root the ways a user or an app update can
#   (no argument)         the checks below, including what the boot repaired
#   --install-init        an /etc/rc.local that runs --init-phase under init
#   --init-phase          pid 1 is SmallCLUE's init: rc ran, runit supervises,
#                         then poweroff -- whose exit is the end of that boot
#
# By hand, in the CLI:
#   tar -cf /tmp/empty.tar -T /dev/null
#   build/tools/fakefsify /tmp/empty.tar /tmp/nroot
#   ISH_NATIVE_ROOT=1 ISH_NATIVE_USER=tester build/ish -f /tmp/nroot /bin/sh /AOK/tests/native_mode.sh
#
# Output is one "native_mode_<case>: PASS" or ": FAIL <why>" line per case.

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH
fails=0

pass() { echo "native_mode_$1: PASS"; }
fail() { echo "native_mode_$1: FAIL $2"; fails=$((fails + 1)); }
# check NAME WANT GOT
check() {
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "want [$2] got [$3]"; fi
}

case "${1:-}" in
--setup-reprovision)
    # A link the build will want back, a file of the user's at a link path,
    # and a stale record entry for a program the build no longer has.
    rm -f /usr/bin/wc
    rm -f /usr/bin/date
    printf '#!/bin/sh\necho mine\n' > /usr/bin/date
    chmod 755 /usr/bin/date
    ln -s /AOK/native/smallclue /usr/bin/gone-from-the-build
    echo /usr/bin/gone-from-the-build >> /etc/aok-native.links
    echo "# my edit" >> /etc/profile
    exit 0
    ;;
--install-init)
    mkdir -p /etc/service/nmdemo
    printf '#!/bin/sh\necho start >> /tmp/nmdemo.starts\nexec sleep 1000\n' > /etc/service/nmdemo/run
    chmod 755 /etc/service/nmdemo/run
    printf '#!/bin/sh\ncase $1 in start) echo ran >> /tmp/nm_rc.count;; esac\n' > /etc/rc.d/S10nmcount
    chmod 755 /etc/rc.d/S10nmcount
    printf '#!/bin/sh\n(sh /AOK/tests/native_mode.sh --init-phase > /tmp/native_mode_init.log 2>&1; poweroff) > /dev/null 2>&1 &\n' > /etc/rc.local
    chmod 755 /etc/rc.local
    rm -f /tmp/nmdemo.starts /tmp/nm_rc.count /tmp/native_mode_init.log
    exit 0
    ;;
--init-phase)
    sleep 2
    check init_is_pid1 init "$(ps -o pid,comm | awk '$1 == 1 { print $2 }')"
    check init_exe /AOK/native/smallclue "$(readlink /proc/1/exe)"
    check rc_ran_once 1 "$(wc -l < /tmp/nm_rc.count | tr -d ' ')"
    pid=$(cat /run/service/nmdemo/pid 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then pass runit_started; else fail runit_started "no pid ($pid)"; fi
    kill "$pid" 2>/dev/null
    sleep 3
    pid2=$(cat /run/service/nmdemo/pid 2>/dev/null)
    if [ -n "$pid2" ] && [ "$pid2" != "$pid" ] && kill -0 "$pid2" 2>/dev/null; then
        pass runit_restarts
    else
        fail runit_restarts "pid before $pid after $pid2"
    fi
    sv down nmdemo > /dev/null 2>&1
    case "$(sv status nmdemo)" in down:*) pass sv_down ;; *) fail sv_down "$(sv status nmdemo)" ;; esac
    sv up nmdemo > /dev/null 2>&1
    case "$(sv status nmdemo)" in run:*) pass sv_up ;; *) fail sv_up "$(sv status nmdemo)" ;; esac
    exit 0
    ;;
esac

# ---- what provisioning made --------------------------------------------------
check bin_merged usr/bin "$(readlink /bin)"
check sh_link /AOK/native/sh "$(readlink /usr/bin/sh)"
check init_link /AOK/native/smallclue "$(readlink /usr/sbin/init)"
check login_link /AOK/native/smallclue "$(readlink /usr/bin/login)"
check sudo_is_setuid_program /AOK/native/sudo "$(readlink /usr/bin/sudo)"
check terminfo_link /AOK/native/libs/terminfo "$(readlink /usr/share/terminfo)"
check os_release aok-native "$(. /etc/os-release; echo "$ID")"
check uname_m aarch64 "$(uname -m)"
check root_owner "0 0" "$(stat -c '%u %g' /)"
check tmp_mode 1777 "$(stat -c '%a' /tmp)"
if [ -x /usr/bin/env ] && [ -x /bin/echo ]; then pass common_paths; else fail common_paths "no /usr/bin/env or /bin/echo"; fi

# ---- what the boot repaired, and what it left alone (--setup-reprovision) -----
if grep -q "^# my edit" /etc/profile; then
    check reprovision_restores /AOK/native/smallclue "$(readlink /usr/bin/wc)"
    check reprovision_keeps_user_file mine "$(/usr/bin/date)"
    if [ -e /usr/bin/gone-from-the-build ]; then fail reprovision_removes_stale "still there"; else pass reprovision_removes_stale; fi
    pass reprovision_keeps_etc_edits
fi

# ---- the everyday account (ISH_NATIVE_USER=tester) ---------------------------
check user_uid 1000 "$(id -u tester 2>/dev/null)"
groups=" $(id -Gn tester 2>/dev/null) "
case "$groups" in
    *" sudo "*) case "$groups" in *" users "*) pass user_groups ;; *) fail user_groups "$groups" ;; esac ;;
    *) fail user_groups "$groups" ;;
esac
check home_mode "700 1000" "$(stat -c '%a %u' /home/tester 2>&1)"

# ---- login, su, sudo ---------------------------------------------------------
got=$(printf 'id -un; pwd; echo "$0"; echo "${FOO-unset}"\n' | FOO=leak login -f tester 2>&1 | tr '\n' ' ')
check login_f "tester /home/tester -zsh unset " "$got"
got=$(su - tester -c 'id -u; echo $HOME' 2>&1 | tr '\n' ' ')
check su_login "1000 /home/tester " "$got"
check sudo_as_user 1000 "$(sudo -n -u tester id -u 2>&1)"
# The root prompt survives a broken account entry: root's shell gone.
cp /etc/passwd /tmp/passwd.keep
sed -i 's|^root:\(.*\):[^:]*$|root:\1:/bin/nonexistent|' /etc/passwd
got=$(echo 'id -u' | login -f root 2>/dev/null | tail -1)
cp /tmp/passwd.keep /etc/passwd
check login_shell_fallback 0 "$got"

# ---- shells and scripts ------------------------------------------------------
check sh_c ok "$(/bin/sh -c 'echo ok')"
printf 'echo no-shebang\n' > /tmp/nm_ns; chmod +x /tmp/nm_ns
check enoexec_script no-shebang "$(/tmp/nm_ns 2>&1)"
printf '#!/usr/bin/env zsh\necho "$ZSH_NAME"\n' > /tmp/nm_ez; chmod +x /tmp/nm_ez
check env_shebang zsh "$(/tmp/nm_ez 2>&1)"
check zsh_compinit 1 "$(zsh -fc 'autoload -Uz compinit && compinit -u -D && echo ${+functions[_ls]}' 2>&1)"

# ---- the applets native mode added -------------------------------------------
check tput_setaf "$(printf '\033[31m')" "$(tput -T xterm-256color setaf 1)"
check tput_colors 256 "$(tput -T screen-256color colors)"
tput -T dumb clear > /dev/null 2>&1
check tput_dumb_clear 2 "$?"
check free_header "               total        used        free      shared  buff/cache   available" \
    "$(free | head -1)"
check printenv_home "$HOME" "$(printenv HOME)"
mkdir -p /tmp/nm_a /tmp/nm_b; echo bound > /tmp/nm_a/f
if mount --bind /tmp/nm_a /tmp/nm_b 2>/dev/null; then
    check mount_bind bound "$(cat /tmp/nm_b/f)"
    umount /tmp/nm_b
    if [ -e /tmp/nm_b/f ]; then fail umount "still bound"; else pass umount; fi
else
    fail mount_bind "mount --bind failed"
fi
check chroot ok "$(chroot / /bin/sh -c 'echo ok' 2>&1)"
# curl's exit status is curl's own: 7, could not connect.
curl -s -o /dev/null http://127.0.0.1:1/ 2>/dev/null
check curl_exit_code 7 "$?"
check curl_write_out 000 "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:1/ 2>/dev/null)"

# ---- git, offline ------------------------------------------------------------
rm -rf /tmp/nm_repo
got=$( (git init -q /tmp/nm_repo && cd /tmp/nm_repo &&
        git config user.name T && git config user.email t@example.com &&
        echo a > f && git add f && git commit -qm one &&
        echo b >> f && git commit -qam two &&
        git checkout -q -b old HEAD~1 && cat f && git checkout -q master 2>/dev/null ||
        git checkout -q main; git log --oneline | wc -l) 2>&1 | tr -d ' ' | tr '\n' ' ')
check git_basic "a 2 " "$got"

# ---- time --------------------------------------------------------------------
# The applet itself: --setup-reprovision put a script of the user's at
# /usr/bin/date.
zone=$(TZ=Europe/London /AOK/native/smallclue date +%Z)
case "$zone" in GMT|BST) pass tz_zoneinfo ;; *) fail tz_zoneinfo "$zone" ;; esac

echo
[ "$fails" -eq 0 ] && echo "native_mode: all cases passed" || echo "native_mode: $fails failed"
exit "$fails"
