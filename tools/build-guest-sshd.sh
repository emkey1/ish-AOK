#!/bin/sh
# Build a STATIC aarch64 sshd for iSH-AOK's native mode.
#
# WHY A GUEST BINARY AND NOT A NATIVE PROGRAM
#
# ssh, scp, sftp and ssh-keygen are native programs -- host code compiled into
# the app. sshd cannot be, and the reason is one line: sshd-session.c's
# privilege-separation monitor does
#
#     pmonitor->m_pid = fork();
#
# a fork-and-continue, with both sides carrying on in sshd's own C code. A
# native program is a function call on a guest task's thread inside the app's
# ONE address space, so it cannot have two sides see different memory at the
# same addresses -- structurally the same wall the bash port hit. Everything
# else in the sshd trio is fork+exec, which AOK's native spawn already is.
# docs/native_mode_plan.md and the native-sshd notes have the history; the
# re-launch port is still the eventual answer.
#
# An ordinary EMULATED guest ELF forks normally, so the problem simply does not
# arise. The cost is connection setup and per-packet crypto speed -- real for
# scp throughput, marginal for typing at a prompt, because the shell on the far
# end is native zsh either way.
#
# WHY STATIC
#
# Native mode has no distribution in it: /lib is empty and there is no libc to
# link against. A dynamically linked sshd (Alpine's openssh-server, say) cannot
# run there at all. Static is the requirement, not an optimisation.
#
# Built from the SAME vendored tree the native client comes from, so client and
# server cannot drift to different OpenSSH versions.
#
# Usage:
#   tools/build-guest-sshd.sh [OUT_DIR]        # default: build/guest-sshd
#
# Needs zig (brew install zig). Produces sshd, sshd-session and sshd-auth.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC="$REPO/deps/smallclue/third-party/openssh"
OUT=${1:-"$REPO/build/guest-sshd"}
WORK="$OUT/.build"

command -v zig >/dev/null 2>&1 || { echo "need zig: brew install zig" >&2; exit 1; }
[ -f "$SRC/configure" ] || { echo "no OpenSSH tree at $SRC (submodules initialised?)" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
# A copy, so a build never dirties the submodule.
( cd "$SRC" && tar -cf - --exclude='.git' . ) | ( cd "$WORK" && tar -xf - )
cd "$WORK"

# AR, RANLIB and NM must be LLVM's, NOT the host's.
#
# macOS ships cctools ar and ranlib, which only understand Mach-O. Handed an
# aarch64 ELF object they warn once -- "archive member 'ssh_api.o' not a
# mach-o file" -- and then produce a 96-byte archive containing nothing but a
# symbol index. The link afterwards fails with 809 undefined symbols, every one
# an ordinary OpenSSH library function (a2port, addr_*, argv_*,
# arc4random_uniform), which reads exactly like a missing source list and is
# not. `zig ar` on that same object gives 60 KB. Worse, the empty archive is
# NEWER than the objects, so make then says "`libssh.a' is up to date" and a
# plain rebuild will not fix it -- hence the rm below.
./configure \
    --host=aarch64-linux-musl \
    CC="zig cc -target aarch64-linux-musl" \
    AR="zig ar" \
    RANLIB="zig ranlib" \
    NM="zig nm" \
    CFLAGS="-O2" \
    LDFLAGS="-static" \
    --without-openssl \
    --without-zlib \
    --without-pam \
    --without-selinux \
    --without-kerberos5 \
    --without-libedit \
    --sysconfdir=/etc/ssh \
    --prefix=/usr \
    > configure.log 2>&1 || { echo "configure failed:" >&2; tail -25 configure.log >&2; exit 1; }

# Each library in its OWN make invocation. All three binaries name libssh.a as
# a prerequisite, and `make -j8 sshd sshd-session sshd-auth` let three jobs race
# to create it: the archive came out empty and the link failed as above. The
# member count is asserted rather than assumed, because an empty archive is the
# failure this whole comment block exists to prevent.
rm -f libssh.a openbsd-compat/libopenbsd-compat.a
make -j8 libssh.a > make.log 2>&1 || { echo "libssh.a failed:" >&2; tail -20 make.log >&2; exit 1; }
( cd openbsd-compat && make -j8 libopenbsd-compat.a ) >> make.log 2>&1 \
    || { echo "libopenbsd-compat.a failed:" >&2; tail -20 make.log >&2; exit 1; }
for a in libssh.a openbsd-compat/libopenbsd-compat.a; do
    n=$(zig ar t "$a" 2>/dev/null | wc -l | tr -d ' ')
    [ "$n" -gt 10 ] || { echo "$a has $n members -- see the AR/RANLIB note in this script" >&2; exit 1; }
done

make -j8 sshd sshd-session sshd-auth >> make.log 2>&1 \
    || { echo "build failed:" >&2; tail -30 make.log >&2; exit 1; }

# sshd execs its helpers from the libexecdir compiled into it, so where these
# land in the guest is not a free choice: sshd in /usr/sbin, sshd-session and
# sshd-auth in /usr/libexec, or sshd exits with "/usr/libexec/sshd-session does
# not exist or is not executable" the moment a client connects. The config
# options SshdSessionPath and SshdAuthPath can override that if one directory
# is wanted instead.
for f in sshd sshd-session sshd-auth; do
    cp "$f" "$OUT/$f"
    chmod 755 "$OUT/$f"
done
rm -rf "$WORK"

echo "built into $OUT:"
for f in sshd sshd-session sshd-auth; do
    printf '  %-13s %9d bytes  %s\n' "$f" "$(wc -c < "$OUT/$f" | tr -d ' ')" \
        "$(file -b "$OUT/$f" 2>/dev/null | cut -c1-54)"
done
cat <<'EOF'

Verified 2026-11-15 by running these in native mode under the CLI: sshd
listened, "Accepted publickey for root", and the session landed on native zsh
with pid 1 being native mode's own init. Privilege separation worked, which is
the whole point of emulating rather than porting.

Note the size: ~22 MB for the three, stripped. They share nearly all of their
code (libssh.a), so folding them into one binary dispatched on argv[0] -- which
is what the native build already does for ssh/scp/sftp/ssh-keygen, see
deps/smallclue/third-party/openssh/README-iSH-AOK.md -- would cut that to
roughly a third.
EOF
