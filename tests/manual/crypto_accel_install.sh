#!/bin/sh
# crypto_accel_install.sh -- regression test for the crypto accelerator
# installer's toolchain and header diagnosis.
#
# The bug, reported on Arch ARM64 with openssl 3.6.3: the installer probed for
# the OpenSSL provider headers by compiling "#include <openssl/core.h>" with
# every byte of compiler output sent to /dev/null, and then reported the single
# cause it had guessed at -- "OpenSSL development headers not found" -- for any
# reason the compile might have failed. On the reporting device the header was
# right there (pacman -Ql listed /usr/include/openssl/core.h, and reinstalling
# openssl changed nothing), so the one message that could have identified the
# real failure was the one message nobody ever saw.
#
# So the property under test is not "the installer works": it is that the
# installer's account of WHY it stopped matches what actually happened, and
# that a probe which passes is followed by a build that gets the same flags.
#
# Everything the installer inspects -- the C compiler, the openssl binary,
# pkg-config, uname -- is a shim on a private PATH, so each scenario is exact,
# nothing on the host is touched, and the test runs anywhere with a POSIX
# shell (host or guest, any arch).
#
#   sh tests/manual/crypto_accel_install.sh [-v]

set -eu

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# Overridable so an old copy can be A/B'd against a fixed one without editing:
#   CRYPTO_ACCEL_INSTALLER=/tmp/old.sh sh tests/manual/crypto_accel_install.sh
INSTALLER=${CRYPTO_ACCEL_INSTALLER:-$HERE/../../opt/AOK/tools/crypto/install-crypto-accel.sh}
PROVIDER=$HERE/../../opt/AOK/tools/crypto/ish_provider.c
[ -f "$INSTALLER" ] || { echo "cannot find install-crypto-accel.sh at $INSTALLER" >&2; exit 1; }
[ -f "$PROVIDER" ]  || { echo "cannot find ish_provider.c at $PROVIDER" >&2; exit 1; }

WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT INT TERM

PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); [ "$VERBOSE" -eq 1 ] && printf '  ok   %s\n' "$*" || true; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$*"; }

# assert <description> <yes|no> <pattern> ; against the last run's output
assert() {
    if grep -q -- "$3" "$WORK/out" 2>/dev/null; then found=yes; else found=no; fi
    if [ "$found" = "$2" ]; then ok "$1"; else
        bad "$1 (expected $2 for '$3', got $found)"
        [ "$VERBOSE" -eq 1 ] && sed 's/^/       | /' "$WORK/out" || true
    fi
}

assert_status() {  # assert_status <description> <zero|nonzero>
    if [ "$STATUS" -eq 0 ]; then got=zero; else got=nonzero; fi
    if [ "$got" = "$2" ]; then ok "$1"; else
        bad "$1 (exit status $STATUS, wanted $2)"
        sed 's/^/       | /' "$WORK/out"
    fi
}

# ------------------------------------------------------------------ the shims

SHIM=$WORK/shim
mkdir -p "$SHIM"

# A compiler real enough for the question being asked: it resolves the
# openssl/* headers a source file includes against its -I flags and the search
# path it reports, so "installed", "not installed" and "installed somewhere I
# do not look" are three genuinely different outcomes rather than three
# spellings of one canned answer. Non-openssl includes are assumed present;
# this test has nothing to say about libc headers.
cat > "$SHIM/cc" <<'SHIM_CC'
#!/bin/sh
[ -n "${FAKE_CC_LOG:-}" ] && printf '%s\n' "$*" >> "$FAKE_CC_LOG"
incs=''; srcs=''; out=''; preprocess=0
while [ $# -gt 0 ]; do
    case $1 in
        -I)  shift; incs="$incs $1" ;;
        -I*) incs="$incs ${1#-I}" ;;
        -o)  shift; out=$1 ;;
        -E)  preprocess=1 ;;
        --version) echo "fake-cc (iSH-AOK test shim) 1.0"; exit 0 ;;
        *.c) srcs="$srcs $1" ;;
        *)   : ;;
    esac
    shift
done

# cc -E -Wp,-v: report the built-in search path, in gcc's and clang's shared
# format (one leading space per directory).
if [ "$preprocess" -eq 1 ]; then
    {
        echo '#include "..." search starts here:'
        echo '#include <...> search starts here:'
        for d in ${FAKE_CC_SYSINC:-}; do echo " $d"; done
        echo 'End of search list.'
    } >&2
    exit 0
fi

if [ "${FAKE_CC_MODE:-normal}" = broken ]; then
    echo "cc: fatal error: cannot execute 'cc1': execvp: No such file or directory" >&2
    echo "compilation terminated." >&2
    exit 1
fi

search=$incs
[ -n "${FAKE_CC_NO_SYSINC:-}" ] || search="$search ${FAKE_CC_SYSINC:-}"
for src in $srcs; do
    for h in $(sed -n 's/^#include <\(openssl\/[^>]*\)>.*/\1/p' "$src"); do
        found=''
        for d in $search; do [ -f "$d/$h" ] && { found=1; break; }; done
        [ -n "$found" ] && continue
        echo "$src:1:10: fatal error: $h: No such file or directory" >&2
        echo "compilation terminated." >&2
        exit 1
    done
    # Every header resolved, and the compile falls over anyway: a toolchain
    # that cannot cope with what it just read. This is the shape the Arch
    # ARM64 report had, and the shape the old probe reported as a missing
    # package.
    if [ "${FAKE_CC_MODE:-normal}" = sabotage ] &&
       grep -q '^#include <openssl/' "$src"; then
        echo "$src:1:0: internal compiler error: Segmentation fault" >&2
        echo "Please submit a full bug report, with preprocessed source if appropriate." >&2
        exit 1
    fi
done
[ -n "$out" ] && : > "$out"
exit 0
SHIM_CC

cat > "$SHIM/openssl" <<'SHIM_SSL'
#!/bin/sh
case "${1:-} ${2:-}" in
    "version -d") echo "OPENSSLDIR: \"$FAKE_SSL_DIR\"" ;;
    "version -m") echo "MODULESDIR: \"$FAKE_SSL_DIR/modules\"" ;;
    *)            echo "OpenSSL 3.6.3 (fake)" ;;
esac
exit 0
SHIM_SSL

# The installer refuses x86 guests by design, so the arch has to be one it
# accepts no matter what the machine running this test happens to be.
cat > "$SHIM/uname" <<'SHIM_UNAME'
#!/bin/sh
[ "${1:-}" = "-m" ] && { echo aarch64; exit 0; }
exec /usr/bin/uname "$@"
SHIM_UNAME

cat > "$SHIM/pkg-config" <<'SHIM_PC'
#!/bin/sh
[ "${1:-}" = "--cflags" ] && [ -n "${FAKE_PKGCONFIG_CFLAGS:-}" ] && {
    printf '%s\n' "$FAKE_PKGCONFIG_CFLAGS"; exit 0; }
exit 1
SHIM_PC

chmod +x "$SHIM/cc" "$SHIM/openssl" "$SHIM/uname" "$SHIM/pkg-config"

# The installer's PATH is this test's to define, down to the ordinary
# utilities: with the host's /usr/bin on it, removing the shim compiler would
# just uncover the host's own cc and the no-compiler scenario would test
# nothing.
BIN=$WORK/bin
mkdir -p "$BIN"
for t in sh sed awk grep cat cp rm mkdir chmod id diff dirname head tail \
         readlink mktemp tr sort wc expr true false ls; do
    p=$(command -v "$t" 2>/dev/null) || continue
    ln -s "$p" "$BIN/$t" 2>/dev/null || true
done

SSLDIR=$WORK/ssl
mkdir -p "$SSLDIR/modules"
cat > "$SSLDIR/openssl.cnf" <<'CNF'
openssl_conf = openssl_init

[openssl_init]
providers = provider_sect

[provider_sect]
default = default_sect

[default_sect]
activate = 1
CNF

# The header set is read out of the provider source, so adding an include there
# cannot silently stop being covered here.
plant_headers() {  # plant_headers <dir>
    mkdir -p "$1/openssl"
    for h in $(sed -n 's/^#include <openssl\/\([^>]*\)>.*/\1/p' "$PROVIDER"); do
        : > "$1/openssl/$h"
    done
}

SYSINC=$WORK/sysinc          # what the fake compiler searches by default
ODDINC=$WORK/opt/ssl/include # an OpenSSL somewhere else entirely
mkdir -p "$SYSINC" "$ODDINC"

# run <env assignments...> -- runs the installer in --dry-run, capturing
# everything it said and its exit status. --dry-run so a scenario that gets all
# the way through changes nothing and needs no root.
run() {
    STATUS=0
    env -i PATH="$SHIM:$BIN" HOME="$WORK" \
        FAKE_SSL_DIR="$SSLDIR" FAKE_CC_LOG="$WORK/cc.log" \
        "$@" \
        /bin/sh "$INSTALLER" --dry-run > "$WORK/out" 2>&1 || STATUS=$?
}

# ------------------------------------------------------------------ scenarios

echo "1. a compiler that cannot compile anything"
: > "$WORK/cc.log"
plant_headers "$SYSINC"
run FAKE_CC_MODE=broken FAKE_CC_SYSINC="$SYSINC"
assert_status "refuses to continue" nonzero
assert "shows the compiler's own error" yes "cannot execute 'cc1'"
assert "names the toolchain as the problem" yes "broken or incomplete toolchain"
assert "does not blame the OpenSSL headers" no "OpenSSL development headers not found"

echo "2. headers installed, compiler fails on them anyway (the reported bug)"
: > "$WORK/cc.log"
run FAKE_CC_MODE=sabotage FAKE_CC_SYSINC="$SYSINC"
assert_status "refuses to continue" nonzero
assert "shows the compiler's own error" yes "internal compiler error"
assert "says the headers are not the problem" yes "are NOT missing"
assert "names where the header actually is" yes "$SYSINC/openssl/core.h"
assert "does not send the user back to the package manager" no "pacman -S openssl"

echo "3. headers genuinely absent"
: > "$WORK/cc.log"
rm -rf "$SYSINC/openssl"
run FAKE_CC_SYSINC="$SYSINC"
assert_status "refuses to continue" nonzero
assert "reports the missing header" yes "OpenSSL development headers not found"
assert "still shows what the compiler said" yes "No such file or directory"
assert "gives the per-distro install hint" yes "pacman -S openssl"
assert "offers the escape hatch for an unusual prefix" yes "ISH_ACCEL_CFLAGS"

echo "4. headers where the compiler looks"
: > "$WORK/cc.log"
plant_headers "$SYSINC"
run FAKE_CC_SYSINC="$SYSINC"
assert_status "gets through to the install step" zero
assert "would install the module" yes "would install"

echo "5. headers only pkg-config knows about"
: > "$WORK/cc.log"
rm -rf "$SYSINC/openssl"
plant_headers "$ODDINC"
run FAKE_CC_SYSINC="$SYSINC" FAKE_PKGCONFIG_CFLAGS="-I$ODDINC"
assert_status "gets through to the install step" zero
# The build must be given the same flags the probe passed with, or a probe
# that succeeds is just a misdiagnosis one step further on.
if grep -- '-shared' "$WORK/cc.log" | grep -q -- "-I$ODDINC"; then
    ok "the build gets the same include flags as the probe"
else
    bad "the build did not get the probe's include flags"
    sed 's/^/       | /' "$WORK/cc.log"
fi

echo "6. headers named by ISH_ACCEL_CFLAGS, no pkg-config"
: > "$WORK/cc.log"
run FAKE_CC_SYSINC="$SYSINC" ISH_ACCEL_CFLAGS="-I$ODDINC"
assert_status "gets through to the install step" zero
assert "would install the module" yes "would install"

echo "7. headers on disk but outside the compiler's search path"
: > "$WORK/cc.log"
plant_headers "$SYSINC"
# The compiler reports $SYSINC as its search path and then does not search it,
# which is a synthesized fault: it is the cheapest way to make the installer
# take its "find the header on disk and retry with -I" branch deterministically.
run FAKE_CC_SYSINC="$SYSINC" FAKE_CC_NO_SYSINC=1
assert_status "recovers and gets through to the install step" zero
if grep -- '-shared' "$WORK/cc.log" | grep -q -- "-I$SYSINC"; then
    ok "the recovered -I reaches the build too"
else
    bad "the recovered -I did not reach the build"
    sed 's/^/       | /' "$WORK/cc.log"
fi

echo "8. the header probe compiles the provider source itself"
# The probe used to be a stand-in translation unit assembled here: the source's
# #include lines grepped out with `>`, then one more line appended with `>>`.
# An iSH-AOK bug left tmpfs ignoring O_APPEND, so on a guest whose /tmp is a
# tmpfs that `>>` wrote at offset 0 and overwrote the first include. The
# compiler then produced a page of errors about a file this script had mangled
# itself and deleted on exit, and the script reported them as an OpenSSL header
# problem -- the same wrong-cause report the whole file exists to prevent, one
# layer down. Probing ish_provider.c directly removes the failure mode outright,
# so hold it there: nothing self-generated, and nothing to append to.
: > "$WORK/cc.log"
plant_headers "$SYSINC"
run FAKE_CC_SYSINC="$SYSINC"
assert_status "gets through to the install step" zero
if grep -- '-fsyntax-only' "$WORK/cc.log" | grep -q 'ish_provider\.c'; then
    ok "the header probe syntax-checks ish_provider.c"
else
    bad "the header probe did not compile ish_provider.c"
    sed 's/^/       | /' "$WORK/cc.log"
fi
if grep -q 'probe-ssl\.c' "$WORK/cc.log"; then
    bad "the probe is a self-generated translation unit again"
    sed 's/^/       | /' "$WORK/cc.log"
else
    ok "no self-generated probe translation unit"
fi

echo "9. the provider source is missing"
# $SRC is resolved from the script's own directory, so a lone copy of the
# installer has nothing to probe and nothing to build. Name that, rather than
# letting it surface as an OpenSSL complaint several steps later.
: > "$WORK/cc.log"
mkdir -p "$WORK/lonely"
cp "$INSTALLER" "$WORK/lonely/install-crypto-accel.sh"
STATUS=0
env -i PATH="$SHIM:$BIN" HOME="$WORK" \
    FAKE_SSL_DIR="$SSLDIR" FAKE_CC_LOG="$WORK/cc.log" FAKE_CC_SYSINC="$SYSINC" \
    /bin/sh "$WORK/lonely/install-crypto-accel.sh" --dry-run > "$WORK/out" 2>&1 || STATUS=$?
assert_status "refuses to continue" nonzero
assert "names the missing source" yes "provider source not found or not readable"
assert "says where it should be" yes "must sit next to this script"
assert "does not blame the OpenSSL headers" no "OpenSSL development headers not found"

echo "10. a dry run on a system with no diffutils"
# Arch's base does not pull in diffutils, which is how this surfaced: the dry
# run printed "diff: command not found" exactly where the change belonged.
: > "$WORK/cc.log"
rm -f "$BIN/diff"
run FAKE_CC_SYSINC="$SYSINC"
assert_status "still gets through to the install step" zero
assert "does not leak the missing tool as an error" no "diff: .*not found"
assert "shows the config it would write instead" yes "default_properties = ?provider=ish"

echo "11. no compiler at all"
: > "$WORK/cc.log"
rm -f "$SHIM/cc"
run FAKE_CC_SYSINC="$SYSINC"
assert_status "refuses to continue" nonzero
assert "reports the missing compiler" yes "no C compiler found"
assert "does not blame the OpenSSL headers" no "OpenSSL development headers not found"

# ------------------------------------------------------------------ verdict

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "PASS ($PASS checks)"
    exit 0
fi
echo "FAIL ($FAIL of $((PASS + FAIL)) checks)"
exit 1
