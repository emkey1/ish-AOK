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
#                         the boot is in utmp and wtmp, then poweroff -- whose
#                         exit is the end of that boot
#   --login-tty           on a terminal (the gate runs it under script(1)):
#                         login -f records the session in utmp and wtmp
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
--foreign-exec)
    # Programs from another root (kernel/foreign_exec.c), in each mode. Run
    # with ISH_FAKE_MNT2=<a glibc root> -- the gate's devuan-arm64-test -- which
    # this places at /AOK/roots/Other the way the app exposes installed roots.
    [ -x /fakemnt2/usr/bin/ls ] || { echo "native_mode_foreign_exec: SKIP no /fakemnt2 root"; exit 0; }
    mount -t tmpfs nm /AOK/roots && mkdir /AOK/roots/Other &&
        mount --bind /fakemnt2 /AOK/roots/Other || { fail foreign_setup "mounts"; exit 1; }
    O=/AOK/roots/Other
    saved=$(head -1 /proc/ish/foreign_exec)
    echo off > /proc/ish/foreign_exec
    if $O/usr/bin/true 2>/dev/null; then fail foreign_off "ran"; else pass foreign_off; fi
    echo root > /proc/ish/foreign_exec
    cd /tmp
    got=$($O/bin/sh -c '. /etc/os-release; echo "$ID $(pwd)"' 2>&1)
    check foreign_root_inside "devuan /" "$got"
    check foreign_root_native_bound zsh "$($O/bin/sh -c '/AOK/native/zsh -fc "echo \$ZSH_NAME"' 2>&1)"
    echo libs > /proc/ish/foreign_exec
    got=$($O/bin/sh -c '. /etc/os-release; echo "$ID"' 2>&1)
    check foreign_libs_here aok-native "$got"
    case "$($O/bin/sh -c 'echo $LD_LIBRARY_PATH' 2>&1)" in
        "$O/"*) pass foreign_libs_ldpath ;; *) fail foreign_libs_ldpath "$($O/bin/sh -c 'echo $LD_LIBRARY_PATH')" ;;
    esac
    # Its locales too: glibc reads /usr/lib/locale, which is this root's.
    # Without LOCPATH the charmap falls back to ASCII and tmux will not start.
    check foreign_libs_locpath "$O/usr/lib/locale" "$($O/bin/sh -c 'echo $LOCPATH' 2>&1)"
    check foreign_libs_locale UTF-8 "$(LANG=C.UTF-8 $O/usr/bin/locale charmap 2>&1)"
    check foreign_libs_user_locpath /mine "$(LOCPATH=/mine $O/bin/sh -c 'echo $LOCPATH' 2>&1)"
    # A root can ask for the booted root's /home and /tmp when it is entered:
    # its /etc/aok-binds, from a fixed set. The companion of a native root
    # does (opt/AOK/docs/native-companion.md). /etc as a whole is not in the set.
    echo root > /proc/ish/foreign_exec
    echo marker > /tmp/nm_binds_marker
    check foreign_binds_before "" "$($O/bin/cat /tmp/nm_binds_marker 2>/dev/null)"
    printf '# test\ntmp\netc\n' > $O/etc/aok-binds
    check foreign_binds_tmp marker "$($O/bin/cat /tmp/nm_binds_marker 2>&1)"
    check foreign_binds_etc_refused 0 "$($O/bin/grep -c aok-native /etc/os-release 2>/dev/null)"
    rm -f /tmp/nm_binds_marker
    # A companion root (opt/AOK/docs/native-companion.md) asks for more: the files
    # that say what this system is called and how it finds others, and its
    # time zone, which is a link into each root's own zoneinfo and so is set
    # there, not bound.
    printf 'tmp\netc/hosts\netc/hostname\ntimezone\n' > $O/etc/aok-binds
    cp /etc/hosts /tmp/nm_hosts.saved
    echo "10.9.8.7 nm-marker-host" >> /etc/hosts
    had_hostname=0; [ -e /etc/hostname ] && { had_hostname=1; cp /etc/hostname /tmp/nm_hostname.saved; }
    echo nm-host > /etc/hostname
    here_zone=$(readlink /etc/localtime 2>/dev/null)
    there_zone=$(readlink $O/etc/localtime 2>/dev/null)
    ln -sf /usr/share/zoneinfo/Asia/Tokyo /etc/localtime
    check foreign_binds_hosts 10.9.8.7 "$($O/bin/sh -c 'grep nm-marker-host /etc/hosts | cut -d" " -f1' 2>&1)"
    check foreign_binds_hostname nm-host "$($O/bin/cat /etc/hostname 2>&1)"
    check foreign_binds_timezone JST "$($O/bin/date +%Z 2>&1)"
    # What is bound in is for the programs inside, and is not there seen from
    # here: `rm -rf /AOK/roots/Devuan` from the booted root once deleted the
    # booted root's own /home through these binds. The marker is in this
    # root's /tmp and hostname; from outside, the other root's own show.
    echo marker > /tmp/nm_scope_marker
    check foreign_scope_inside marker "$($O/bin/cat /tmp/nm_scope_marker 2>&1)"
    check foreign_scope_outside_dir "" "$(cat $O/tmp/nm_scope_marker 2>/dev/null)"
    if [ "$(cat $O/etc/hostname 2>/dev/null)" = nm-host ]; then fail foreign_scope_outside_file "bound file seen from outside"; else pass foreign_scope_outside_file; fi
    rm -f $O/tmp/nm_scope_marker 2>/dev/null
    check foreign_scope_rm_spares marker "$(cat /tmp/nm_scope_marker 2>&1)"
    rm -f /tmp/nm_scope_marker
    # The cwd carries in where a shared directory shows it there too, and
    # is the root's / otherwise.
    mkdir -p /tmp/nm_cwd && cd /tmp/nm_cwd
    check foreign_cwd_shared /tmp/nm_cwd "$($O/bin/pwd 2>&1)"
    cd /usr
    check foreign_cwd_unshared / "$($O/bin/pwd 2>&1)"
    cd /tmp
    # A script that lives in another root runs inside it, named by its path
    # there -- whether or not this root has the interpreter its #! names.
    # Both do here: /bin/sh and /usr/bin/env are native programs.
    printf '#!/bin/sh\n. /etc/os-release; echo "$ID $0 $1"\n' > $O/usr/local/bin/nm_script
    printf '#!/usr/bin/env perl\nprint "perl $0\\n";\n' > $O/usr/local/bin/nm_env
    chmod 755 $O/usr/local/bin/nm_script $O/usr/local/bin/nm_env
    check foreign_script_inside "devuan /usr/local/bin/nm_script a" "$($O/usr/local/bin/nm_script a 2>&1)"
    check foreign_script_env "perl /usr/local/bin/nm_env" "$($O/usr/local/bin/nm_env 2>&1)"
    echo libs > /proc/ish/foreign_exec
    check foreign_libs_script_here "aok-native $O/usr/local/bin/nm_script a" "$($O/usr/local/bin/nm_script a 2>&1)"
    echo root > /proc/ish/foreign_exec
    rm -f $O/usr/local/bin/nm_script $O/usr/local/bin/nm_env
    umount $O/etc/hosts $O/etc/hostname 2>/dev/null
    cat /tmp/nm_hosts.saved > /etc/hosts
    if [ "$had_hostname" = 1 ]; then cat /tmp/nm_hostname.saved > /etc/hostname; else rm -f /etc/hostname; fi
    if [ -n "$here_zone" ]; then ln -sf "$here_zone" /etc/localtime; else rm -f /etc/localtime; fi
    [ -z "$there_zone" ] || ln -sf "$there_zone" $O/etc/localtime
    rm -rf /tmp/nm_cwd /tmp/nm_hosts.saved /tmp/nm_hostname.saved $O/etc/aok-binds
    echo "$saved" > /proc/ish/foreign_exec
    exit 0
    ;;
--login-tty)
    # Run as the shell login -f started, on the terminal script(1) gave it.
    check login_tty_who tester "$(who | awk '{ print $1 }' | sort -u | tr -d '\n')"
    check login_tty_who_am_i "$(tty | sed 's|^/dev/||')" "$(who am i | awk '{ print $2 }')"
    check login_tty_users tester "$(users)"
    check login_tty_wtmp yes "$(who /var/log/wtmp | grep -q '^tester ' && echo yes || echo no)"
    # login gives the terminal to the user, as util-linux's does; last then
    # knows the session is live (no /proc/<pid>/loginuid here: it goes by the
    # terminal's owner).
    check login_tty_owner "$(id -u)" "$(stat -c %u "$(tty)")"
    check login_tty_last_live yes "$(last -n 1 tester | grep -q 'still logged in' && echo yes || echo no)"
    exit 0
    ;;
--init-phase)
    sleep 2
    # The boot as init recorded it (SmallCLUE's init_app.c, utmp_rec.c).
    case "$(who -b)" in *"system boot"*) pass utmp_boot ;; *) fail utmp_boot "who -b: $(who -b)" ;; esac
    case "$(who -r)" in *"run-level 2"*) pass utmp_runlevel ;; *) fail utmp_runlevel "who -r: $(who -r)" ;; esac
    case "$(who -b /var/log/wtmp)" in *"system boot"*) pass wtmp_boot ;; *) fail wtmp_boot "no boot record in wtmp" ;; esac
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
# ip and ifconfig are on every platform now, not the PSCAL app alone: they read
# the host's interfaces as the kernel's netlink does, and routes from /proc/net.
check ip_link /AOK/native/smallclue "$(readlink /usr/bin/ip)"
check ifconfig_link /AOK/native/smallclue "$(readlink /usr/bin/ifconfig)"
check ip_lo_addr "127.0.0.1/8" "$(ip -4 -br addr show lo0 | awk '{print $3}')"
check ifconfig_lo_flags "lo0: flags=73<UP,LOOPBACK,RUNNING>" \
    "$(ifconfig lo0 | head -1 | sed 's/  mtu.*//; s/,MULTICAST//; s/=4169/=73/')"
check ip_route_is_read "yes" "$(ip route | grep -q ' dev lo0' && echo yes || echo no)"
got=$(ifconfig lo0 down 2>&1); rc=$?
check ifconfig_refuses "SIOCSIFFLAGS: Operation not permitted|255" "$got|$rc"
got=$(ifconfig nosuch0 2>&1); rc=$?
check ifconfig_missing "nosuch0: error fetching interface information: Device not found|1" "$got|$rc"
check sudo_is_setuid_program /AOK/native/sudo "$(readlink /usr/bin/sudo)"
check terminfo_link /AOK/native/libs/terminfo "$(readlink /usr/share/terminfo)"
check os_release aok-native "$(. /etc/os-release; echo "$ID")"
# w, last and finger (SmallCLUE login_apps.c), linked like every applet.
check w_header "load average" "$(w | head -1 | grep -o 'load average')"
check finger_long "Login: tester" "$(finger tester 2>/dev/null | head -1 | cut -c1-13)"
check finger_missing "finger: nosuch0: no such user." "$(finger nosuch0 2>&1)"
got=$(last -f /dev/null 2>&1); rc=$?
check last_empty_file "null begins|0" "$(printf '%s' "$got" | grep -o 'null begins')|$rc"
# The SmallCLUE this build carries, "<commit> <date>", in place of the old
# "(no distribution)"; "unknown" means the build could not ask git.
got=$(. /etc/os-release; echo "$PRETTY_NAME|$VERSION")
case "$got" in
    "AOK-iSH Native (SmallCLUE "[0-9a-f]*" "[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]")|SmallCLUE "*)
        pass os_release_smallclue ;;
    *) fail os_release_smallclue "got [$got]" ;;
esac
check uname_m aarch64 "$(uname -m)"
check uname_o AOK/Linux "$(uname -o)"
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
# A second account, as Settings -> Default User -> Add User makes one (the
# gate's main run passes ISH_NATIVE_USER=tester2): the next uid, and the same
# groups, home and sudo as the first.
if grep -q '^tester2:' /etc/passwd; then
    check user2_uid 1001 "$(id -u tester2 2>/dev/null)"
    check user2_gid 1001 "$(id -g tester2 2>/dev/null)"
    groups=" $(id -Gn tester2 2>/dev/null) "
    case "$groups" in
        *" sudo "*) case "$groups" in *" users "*) pass user2_groups ;; *) fail user2_groups "$groups" ;; esac ;;
        *) fail user2_groups "$groups" ;;
    esac
    check user2_home_mode "700 1001" "$(stat -c '%a %u' /home/tester2 2>&1)"
    check user2_shadow 1 "$(grep -c '^tester2:\$6\$' /etc/shadow)"
    check user1_still_default 1000 "$(id -u tester 2>/dev/null)"
fi

# ---- login, su, sudo ---------------------------------------------------------
got=$(printf 'id -un; pwd; echo "$0"; echo "${FOO-unset}"\n' | FOO=leak login -f tester 2>&1 | tr '\n' ' ')
check login_f "tester /home/tester -zsh unset " "$got"
got=$(su - tester -c 'id -u; echo $HOME' 2>&1 | tr '\n' ' ')
check su_login "1000 /home/tester " "$got"
check sudo_as_user 1000 "$(sudo -n -u tester id -u 2>&1)"
if grep -q '^tester2:' /etc/passwd; then
    check user2_login_f "tester2 /home/tester2" "$(printf 'id -un; pwd\n' | login -f tester2 2>&1 | tr '\n' ' ' | sed 's/ $//')"
    check user2_sudo_as 1001 "$(sudo -n -u tester2 id -u 2>&1)"
    # sudo -S reads the password from standard input (the gate gives tester2
    # ISH_NATIVE_PASSWORD=aok-test); -k alone is a whole command.
    check sudo_S_right root "$(su tester2 -c 'echo aok-test | sudo -S -k id -un' 2>/dev/null)"
    su tester2 -c 'echo wrong | sudo -S -k id -un' > /dev/null 2>&1
    check sudo_S_wrong 1 "$?"
    check sudo_S_eof "sudo: no password was provided" \
        "$(su tester2 -c 'sudo -S -k id -un < /dev/null' 2>&1 | sed 's/^\[sudo\] password for tester2: //')"
    su tester2 -c 'sudo -k' > /dev/null 2>&1
    check sudo_k_alone 0 "$?"
    # -p sets the prompt: '' prints none (the usual scripted form), and the
    # escapes expand as sudo(8)'s do.
    got=$(su tester2 -c "echo aok-test | sudo -S -k -p '' id -un 2>/tmp/sudo_p.err")
    check sudo_p_empty "root|" "$got|$(cat /tmp/sudo_p.err)"
    check sudo_p_escapes "P[tester2 tester2 root %]" \
        "$(su tester2 -c "echo aok-test | sudo -S -k -p 'P[%p %u %U %%]' true" 2>&1)"
    check sudo_p_joined "Q:" "$(su tester2 -c 'echo aok-test | sudo -S -k -pQ: true' 2>&1)"
    rm -f /tmp/sudo_p.err
fi
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
