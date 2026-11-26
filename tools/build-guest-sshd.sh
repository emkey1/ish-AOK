#!/bin/sh
# Build a STATIC aarch64 sshd for AOK-Workstation's native mode.
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
# WHY -DAOK_GUEST_ACCEL
#
# The crypto speed, at least, is recoverable. With it, cipher-chachapoly.c asks
# AOK's crypto accelerator (syscall 0xacc0, kernel/ish_accel.c) for each
# ChaCha20 and Poly1305 step and runs OpenSSH's own code when it declines --
# Settings' Crypto Accel switch off, or not AOK at all. Only this build may set
# it: the native client compiles the same tree as host code, where 0xacc0 is
# not AOK's. The fork's aok-guest-accel.h has the rest, and
# tests/manual/crypto/openssh_chachapoly_test.c checks the two paths agree.
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
# WHY THREE BINARIES AND NOT ONE
#
# The native client build folds four programs into one binary dispatched on
# argv[0] (README-iSH-AOK.md, "One binary, four programs"), and the same thing
# was tried here. It does not transfer, and the reason is worth writing down so
# nobody spends the afternoon again.
#
# The three sshd roles are not four separate programs sharing a library: they
# are three different implementations of the SAME entry points, linked against
# one shared set of objects. Of the 23 globals that collide in a union link,
# eight are functions that each role defines differently and that SHARED code
# calls -- cleanup_exit (all three, 5/27/5 lines), and get_hostkey_by_index,
# get_hostkey_index, get_hostkey_private_by_type, get_hostkey_public_by_index,
# get_hostkey_public_by_type and mm_is_monitor between sshd-session and
# sshd-auth. Only sshd_hostkey_sign is byte-identical in both. Folding means a
# runtime trampoline per entry point, dispatching on which role this process
# is, plus renaming 16 definitions and merging 15 data symbols -- and redoing
# that audit at every OpenSSH update, against functions upstream is actively
# diverging (get_hostkey_public_by_type is 5 lines in one file and 36 in the
# other).
#
# And it buys almost nothing, because what ships is compressed:
#
#     three binaries, raw                21 318 144
#     three binaries, LZMA                2 486 672   <- what a bundle costs
#     sshd-session alone, raw             8 033 248   <- a FLOOR for any fold
#     sshd-session alone, LZMA            1 875 548
#
# So the whole fold is worth at best 0.6 MB of download. Ship the three, in one
# LZMA stream, and leave OpenSSH's own structure alone.
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

# AR and RANLIB must be LLVM's, NOT the host's. (There is no `zig nm`, and
# OpenSSH's configure never uses NM, so nothing needs to stand in for it.)
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
    CFLAGS="-O2 -ffunction-sections -fdata-sections -DAOK_GUEST_ACCEL" \
    LDFLAGS="-static -Wl,--gc-sections" \
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

# One LZMA stream, which is what the app ships and what it decompresses on the
# way into a native root. /usr/bin/compression_tool is base macOS -- no brew,
# no xz -- and its output is exactly what Compression.framework's
# COMPRESSION_LZMA decodes, so the app needs no third-party decompressor. The
# ratio is the same as xz -9e (2.49 MB for the three).
#
# A plain tar inside, deliberately: the thing that unpacks these in the app
# wants names, modes and sizes, and tar is the only archive format every host
# in this project can already write. --format=ustar is NOT optional: bsdtar
# defaults to pax, which interleaves 'x' extended headers, and the unpacker in
# kernel/guest_sshd.c reads plain ustar (it skips 'x' records, but there is no
# reason to make it rely on that).
( cd "$OUT" && tar --format=ustar -cf guest-sshd-aarch64.tar \
    sshd sshd-session sshd-auth )
/usr/bin/compression_tool -encode -a lzma \
    -i "$OUT/guest-sshd-aarch64.tar" \
    -o "$OUT/guest-sshd-aarch64.tar.lzma"
rm -f "$OUT/guest-sshd-aarch64.tar"

echo "built into $OUT:"
for f in sshd sshd-session sshd-auth; do
    printf '  %-24s %9d bytes  %s\n' "$f" "$(wc -c < "$OUT/$f" | tr -d ' ')" \
        "$(file -b "$OUT/$f" 2>/dev/null | cut -c1-54)"
done
printf '  %-24s %9d bytes  LZMA, for Compression.framework\n' \
    guest-sshd-aarch64.tar.lzma \
    "$(wc -c < "$OUT/guest-sshd-aarch64.tar.lzma" | tr -d ' ')"
cat <<'EOF'

Verified 2026-11-15 in native mode under the CLI, both before and after the
gc-sections flags went in: `sshd -T` clean, "Server listening", "Accepted
publickey for root", and the session landed on native zsh as uid 0 with pid 1
being native mode's own init. Privilege separation worked, which is the whole
point of emulating rather than porting.

WHAT A ROOT NEEDS BEFORE sshd WILL EVEN START

Two of these are hard refusals, printed and then exit -- not warnings:

  * an `sshd` account in /etc/passwd and /etc/group ("Privilege separation
    user sshd does not exist"), home /var/empty, shell /sbin/nologin;
  * /var/empty, owned by root and not group- or world-writable
    ("/var/empty must be owned by root and not group or world-writable");
  * /etc/ssh/sshd_config, a host key (ssh-keygen is a native program, so
    `ssh-keygen -A` works in native mode), and /run for the pid file.

sshd execs its helpers by the paths compiled into it, so install them as
/usr/sbin/sshd, /usr/libexec/sshd-session and /usr/libexec/sshd-auth, or
override both with SshdSessionPath and SshdAuthPath in sshd_config.

To reproduce the above locally, give the CLI -f (fakefs), NOT -r: -r is a
realfs root, where chown fails, so /var/empty cannot be owned by root and sshd
refuses to start. The app's own boot root is a fakefs (mount_root(&fakefs, ...)
in app/AppDelegate.m), so this is a quirk of the local rig only.
EOF
