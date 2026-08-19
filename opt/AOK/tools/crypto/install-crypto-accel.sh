#!/bin/sh
# install-crypto-accel.sh -- build and install the iSH-AOK OpenSSL crypto
# accelerator provider into this root filesystem, so ssh/scp/sftp and anything
# else built on OpenSSL transparently use the host-native cipher instead of
# emulating it instruction by instruction.
#
#   sh /AOK/tools/crypto/install-crypto-accel.sh            # install
#   sh /AOK/tools/crypto/install-crypto-accel.sh --uninstall # revert
#   sh /AOK/tools/crypto/install-crypto-accel.sh --dry-run   # show, change nothing
#
# Editing openssl.cnf badly can make every OpenSSL program on the system stop
# working, including ssh -- the stock config says so itself. So this script
# never edits the live file: it builds a candidate, proves the candidate works
# (default provider still active, RNG still fetchable), and only then moves it
# into place, restoring the backup automatically if the installed file somehow
# fails the same checks.

set -eu

TAG='ish-accel-managed'
BEGIN='# >>> iSH-AOK crypto accelerator >>>'
END='# <<< iSH-AOK crypto accelerator <<<'

DO_UNINSTALL=0
DRY_RUN=0
VERBOSE=0
DEST=''

say()  { printf '%s\n' "$*"; }
vsay() { [ "$VERBOSE" -eq 1 ] && printf '  %s\n' "$*" || true; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Options:
  --uninstall     remove the provider from openssl.cnf and delete the module
  --dry-run       report what would happen, change nothing
  --dest DIR      install the module into DIR (default: OpenSSL's MODULESDIR)
  -v, --verbose   show each step
  -h, --help      this text

Environment:
  CC                 compiler to build the provider with (default: cc, gcc, clang)
  ISH_ACCEL_CFLAGS   extra compiler flags, for an OpenSSL whose headers are
                     somewhere that compiler does not search by default
                     (e.g. ISH_ACCEL_CFLAGS=-I/opt/ssl/include)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --uninstall) DO_UNINSTALL=1 ;;
        --dry-run)   DRY_RUN=1 ;;
        --dest)      shift; [ $# -gt 0 ] || die "--dest needs a directory"; DEST=$1 ;;
        -v|--verbose) VERBOSE=1 ;;
        -h|--help)   usage; exit 0 ;;
        *)           die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

SRCDIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
SRC="$SRCDIR/ish_provider.c"

# ---------------------------------------------------------------- preflight

command -v openssl >/dev/null 2>&1 || die "no openssl binary in PATH"

# Ask OpenSSL itself where things live; guessing per-distro paths is how you
# end up installing a module the library never looks at.
OPENSSLDIR=$(openssl version -d | sed 's/^OPENSSLDIR: *"\(.*\)"$/\1/')
MODULESDIR=$(openssl version -m | sed 's/^MODULESDIR: *"\(.*\)"$/\1/')
[ -n "$OPENSSLDIR" ] || die "could not determine OPENSSLDIR from 'openssl version -d'"
[ -n "$DEST" ] || DEST="$MODULESDIR"
[ -n "$DEST" ] || die "could not determine a module directory; pass --dest DIR"

CNF="$OPENSSLDIR/openssl.cnf"
[ -f "$CNF" ] || die "no openssl.cnf at $CNF"
# Follow the symlink so the backup and the edit land on the real file.
if [ -L "$CNF" ]; then
    REAL=$(readlink -f "$CNF" 2>/dev/null || true)
    [ -n "$REAL" ] && CNF="$REAL"
fi
MODULE="$DEST/ish.so"

vsay "OPENSSLDIR = $OPENSSLDIR"
vsay "config     = $CNF"
vsay "module     = $MODULE"

need_root() {
    [ "$(id -u)" = "0" ] && return 0
    die "must run as root to modify $CNF (try: sudo sh $0 $*)"
}

# diffutils is not everywhere: Arch's base does not pull it in. A dry run whose
# whole job is to show you the change is no use if it prints "diff: command not
# found" where the change should be, so fall back to the finished file.
show_change() {  # show_change <before> <after>
    if command -v diff >/dev/null 2>&1; then
        diff -u "$1" "$2" || true
    else
        say "(no diff installed, so here is the file as it would be written)"
        sed 's/^/  /' "$2"
    fi
}

# ---------------------------------------------------------------- uninstall

strip_config() {  # strip_config <infile> <outfile>
    awk -v tag="$TAG" -v b="$BEGIN" -v e="$END" '
        index($0, b) { inblock = 1; next }
        index($0, e) { inblock = 0; next }
        inblock      { next }
        index($0, tag) { next }
        { print }
    ' "$1" > "$2"
}

if [ "$DO_UNINSTALL" -eq 1 ]; then
    if ! grep -q "$TAG" "$CNF" 2>/dev/null && ! grep -q "iSH-AOK crypto accelerator" "$CNF" 2>/dev/null; then
        say "crypto accelerator is not installed in $CNF; nothing to do"
        [ -e "$MODULE" ] && say "note: $MODULE still present (not referenced)"
        exit 0
    fi
    TMP=$(mktemp) || die "mktemp failed"
    trap 'rm -f "$TMP"' EXIT INT TERM
    strip_config "$CNF" "$TMP"
    if [ "$DRY_RUN" -eq 1 ]; then
        say "[dry-run] would restore $CNF to:"
        show_change "$CNF" "$TMP"
        exit 0
    fi
    need_root --uninstall
    if ! OPENSSL_CONF="$TMP" openssl rand -hex 4 >/dev/null 2>&1; then
        die "stripped config fails its sanity check; leaving $CNF alone"
    fi
    cp "$CNF" "$CNF.ish-accel.prev"
    cat "$TMP" > "$CNF"
    rm -f "$MODULE"
    say "removed the accelerator from $CNF (previous kept at $CNF.ish-accel.prev)"
    say "removed $MODULE"
    exit 0
fi

# ---------------------------------------------------------------- guest checks

ARCH=$(uname -m)
case "$ARCH" in
    aarch64|arm64|riscv64)
        ;;
    i386|i486|i586|i686|x86_64|amd64)
        die "the accelerator is not offered for $ARCH guests by design.
       x86 guests already run these ciphers at reasonable speed, so there is
       nothing to win here. Nothing to install."
        ;;
    *)
        say "warning: unrecognised guest arch '$ARCH'; continuing, but the"
        say "         accelerator syscall may not be wired for it"
        ;;
esac

TOGGLE=/proc/ish/defaults/enable_crypto_accel
if [ -r "$TOGGLE" ]; then
    if [ "$(cat "$TOGGLE" 2>/dev/null)" != "true" ]; then
        say "warning: 'Enable Crypto Accel' is OFF in iSH-AOK Settings."
        say "         The provider will install, fail its self-test, and decline"
        say "         every algorithm -- OpenSSL just falls back to software, so"
        say "         nothing breaks, but you get no speedup until you turn the"
        say "         toggle on (Settings, then restart the affected programs)."
    else
        vsay "toggle     = enabled"
    fi
else
    say "warning: $TOGGLE not readable; are you running under iSH-AOK?"
fi

# Both the header probe and the build compile this file, so it has to be
# readable, not merely present.
{ [ -f "$SRC" ] && [ -r "$SRC" ]; } ||
    die "provider source not found or not readable at $SRC
       ish_provider.c must sit next to this script."

# ---------------------------------------------------------------- toolchain

# One scratch directory for every temporary file, created before the first
# probe so no step has to invent a path in /tmp of its own.
WORK=$(mktemp -d) || die "mktemp -d failed"
trap 'rm -rf "$WORK"' EXIT INT TERM

CC=${CC:-}
if [ -n "$CC" ]; then
    command -v "$CC" >/dev/null 2>&1 || die "\$CC is set to '$CC', which is not executable"
else
    for c in cc gcc clang; do
        command -v "$c" >/dev/null 2>&1 && { CC=$c; break; }
    done
fi
[ -n "$CC" ] || die "no C compiler found. Install one first:
       Alpine: apk add build-base openssl-dev
       Debian/Devuan/Ubuntu: apt install build-essential libssl-dev
       Arch: pacman -S base-devel openssl"
vsay "compiler   = $(command -v "$CC") ($("$CC" --version 2>/dev/null | head -1))"

# Every probe below shows what the compiler actually said. A check that
# swallows the error it tripped over can only report its own guess at the
# cause, and a wrong guess sends you off reinstalling a package that was never
# missing: that is exactly how a present openssl/core.h got reported as absent.
show_cc_err() {  # show_cc_err <file>
    [ -s "$1" ] || return 0
    printf '%s\n' "what the compiler said:" >&2
    sed 's/^/  /' "$1" >&2
}

# Probe 1: does the toolchain work at all? An include-free translation unit can
# only fail if the compiler itself is broken, and saying so plainly beats
# blaming OpenSSL for it.
printf 'int ish_accel_probe;\n' > "$WORK/probe-cc.c"
if ! "$CC" -fsyntax-only "$WORK/probe-cc.c" 2> "$WORK/probe-cc.err"; then
    show_cc_err "$WORK/probe-cc.err"
    die "$CC cannot compile a trivial file, so nothing here would have worked.
       This is a broken or incomplete toolchain, not a missing OpenSSL:
       Alpine: apk add build-base
       Debian/Devuan/Ubuntu: apt install build-essential
       Arch: pacman -S base-devel"
fi

# Where the compiler itself looks, plus the usual out-of-the-way prefixes, so
# that "not installed" and "installed somewhere this compiler never looks" stay
# distinguishable.
find_openssl_include() {
    for d in $("$CC" -E -Wp,-v -xc /dev/null 2>&1 >/dev/null | sed -n 's/^ \{1,\}//p') \
             /usr/include /usr/local/include /usr/local/ssl/include /opt/include
    do
        if [ -f "$d/openssl/core.h" ]; then printf '%s\n' "$d"; return 0; fi
    done
    return 1
}

# Probe 2: the provider headers, with exactly the flags AND exactly the source
# the build below uses, because a probe that passes on anything different from
# the build just moves the misdiagnosis one step later.
#
# This syntax-checks ish_provider.c itself rather than a stand-in translation
# unit assembled here. Assembling one was wrong twice over: it could drift from
# the real source, and it made this script's own output the thing under test.
# That is not hypothetical -- an iSH-AOK bug (fixed alongside this) left tmpfs
# ignoring O_APPEND, so the `>>` that used to finish the generated probe wrote
# at offset 0 and overwrote the first line of it. The compiler then reported a
# page of errors about a file this script had mangled itself and deleted on
# exit, and the script reported them as an OpenSSL header problem. Compiling
# $SRC has no such failure mode: every diagnostic names a real line of a file
# that is still on disk afterwards, and there is nothing left to append to.
# ($SRC was checked for readability up with the other preconditions.)
SSL_CFLAGS=''
if command -v pkg-config >/dev/null 2>&1; then
    SSL_CFLAGS=$(pkg-config --cflags libcrypto 2>/dev/null || true)
fi
set -- $SSL_CFLAGS ${ISH_ACCEL_CFLAGS:-}   # collapse, so no flags stays no flags
SSL_CFLAGS=$*

if ! "$CC" -fsyntax-only $SSL_CFLAGS "$SRC" 2> "$WORK/probe-ssl.err"; then
    # The headers may simply sit outside the compiler's search path (a
    # hand-built OpenSSL under /usr/local is the usual reason), so find them on
    # disk and retry with an explicit -I before concluding anything.
    INC=$(find_openssl_include || true)
    if [ -n "$INC" ] && "$CC" -fsyntax-only "-I$INC" $SSL_CFLAGS "$SRC" \
            2> "$WORK/probe-ssl.err2"; then
        SSL_CFLAGS="-I$INC $SSL_CFLAGS"
        vsay "headers    = $INC (needed an explicit -I)"
    else
        show_cc_err "$WORK/probe-ssl.err"
        if [ -n "$INC" ]; then
            die "the OpenSSL headers are NOT missing: $INC/openssl/core.h
       is right there, and $CC failed to compile $SRC against it anyway. The
       compiler output above is the real failure; reinstalling openssl will
       not change it. Please report that output."
        fi
        die "OpenSSL development headers not found (openssl/core.h). Install them:
       Alpine: apk add openssl-dev
       Debian/Devuan/Ubuntu: apt install libssl-dev
       Arch: pacman -S openssl
       If they are installed somewhere this compiler does not search, name it:
       ISH_ACCEL_CFLAGS=-I/path/to/include sh $0"
    fi
fi
vsay "headers    = ok${SSL_CFLAGS:+ (flags:$SSL_CFLAGS)}"

# ---------------------------------------------------------------- build

say "building the provider from $SRC ..."
if ! "$CC" -O2 -fPIC -shared $SSL_CFLAGS -o "$WORK/ish.so" "$SRC" 2> "$WORK/build.err"; then
    cat "$WORK/build.err" >&2
    die "provider build failed"
fi
vsay "built $WORK/ish.so"

# ---------------------------------------------------------------- config edit

# Sections we redeclare must be edited IN PLACE: a later [section] in an
# OpenSSL config replaces the earlier one rather than merging into it, so
# appending "[provider_sect] ish = ish_sect" silently drops the default
# provider and takes the RNG with it.
build_config() {  # build_config <infile> <outfile> <module-path>
    strip_config "$1" "$WORK/stripped.cnf"
    # Everything hangs off a top-level "openssl_conf" pointer, and it has to
    # appear before the first section header. Some distros ship without one.
    if ! grep -qE '^[ \t]*openssl_conf[ \t]*=' "$WORK/stripped.cnf"; then
        { printf '%s\n' "openssl_conf = openssl_init  # $TAG"; cat "$WORK/stripped.cnf"; } \
            > "$WORK/stripped2.cnf"
        mv "$WORK/stripped2.cnf" "$WORK/stripped.cnf"
    fi
    awk -v mod="$3" -v tag="$TAG" -v b="$BEGIN" -v e="$END" '
        function section_of(line,   s) {
            if (line !~ /^[ \t]*\[/) return ""
            s = line; sub(/^[ \t]*\[[ \t]*/, "", s); sub(/[ \t]*\].*$/, "", s)
            return s
        }
        {
            sec = section_of($0)
            if (sec != "") {
                cur = sec
                print
                if (sec == "openssl_init") { print "alg_section = ish_evp_properties  # " tag; seen_init = 1 }
                else if (sec == "provider_sect") { print "ish = ish_sect  # " tag; seen_prov = 1 }
                next
            }
            # Track whether the default provider is already explicitly activated.
            if (cur == "default_sect" && $0 ~ /^[ \t]*activate[ \t]*=/) def_active = 1
            print
        }
        END {
            print ""
            print b
            if (!seen_init) { print "[openssl_init]"; print "providers = provider_sect"; print "alg_section = ish_evp_properties" }
            if (!seen_prov) { print "[provider_sect]"; print "default = default_sect"; print "ish = ish_sect" }
            print ""
            print "[ish_evp_properties]"
            print "# \"?\" makes this a preference, not a requirement: when the accelerator"
            print "# is unavailable the provider declines and OpenSSL uses its own code."
            print "default_properties = ?provider=ish"
            print ""
            print "[ish_sect]"
            print "module = " mod
            print "activate = 1"
            if (!def_active) {
                print ""
                print "# Explicitly activating any provider disables the implicit activation"
                print "# of the default one, which would leave OpenSSL without a working RNG."
                print "[default_sect]"
                print "activate = 1"
            }
            print e
        }
    ' "$WORK/stripped.cnf" > "$2"
}

# The candidate config must pass every check BEFORE it goes anywhere near the
# live path. These are the two failures that actually bite: the ish provider
# not loading at all, and the default provider getting deactivated.
check_config() {  # check_config <cnf> ; prints why it failed
    if ! OPENSSL_CONF="$1" openssl list -providers 2>/dev/null | grep -q '^  default$'; then
        echo "default provider is not active"; return 1
    fi
    if ! OPENSSL_CONF="$1" openssl list -providers 2>/dev/null | grep -q '^  ish$'; then
        echo "ish provider did not load"; return 1
    fi
    if ! OPENSSL_CONF="$1" openssl rand -hex 8 >/dev/null 2>&1; then
        echo "RNG unavailable (openssl rand failed)"; return 1
    fi
    if ! OPENSSL_CONF="$1" openssl dgst -sha256 /dev/null >/dev/null 2>&1; then
        echo "digests unavailable (openssl dgst failed)"; return 1
    fi
    return 0
}

build_config "$CNF" "$WORK/candidate.cnf" "$MODULE"

if [ "$DRY_RUN" -eq 1 ]; then
    say "[dry-run] would install $WORK/ish.so -> $MODULE"
    say "[dry-run] would apply this change to $CNF:"
    show_change "$CNF" "$WORK/candidate.cnf"
    exit 0
fi

need_root

mkdir -p "$DEST"
cp "$WORK/ish.so" "$MODULE"
chmod 0644 "$MODULE"
vsay "installed $MODULE"

# Validate the candidate while the live config is still untouched.
if ! why=$(check_config "$WORK/candidate.cnf"); then
    rm -f "$MODULE"
    die "candidate config rejected: $why
       $CNF was NOT modified and the module was removed."
fi
vsay "candidate config passed all checks"

BACKUP="$CNF.ish-accel.bak"
[ -f "$BACKUP" ] || cp "$CNF" "$BACKUP"
cat "$WORK/candidate.cnf" > "$CNF"

# Belt and braces: re-check what is actually on disk now, and roll back if the
# installed file behaves differently from the candidate for any reason.
if ! why=$(check_config "$CNF"); then
    cat "$BACKUP" > "$CNF"
    rm -f "$MODULE"
    die "installed config failed its check ($why); restored $BACKUP"
fi

say "installed: $MODULE"
say "configured: $CNF (original saved as $BACKUP)"
say ""
say "verifying ..."
OPENSSL_CONF="$CNF" openssl list -providers 2>/dev/null | sed 's/^/  /'

# Which algorithms the provider actually took on. ChaCha20 and AES-256-GCM are
# probed independently (the host can have one and not the other), so this is
# the crisp answer to "did it work" -- an empty list means the provider loaded
# and then declined everything.
say ""
say "algorithms taken over by the accelerator:"
offered=$(OPENSSL_CONF="$CNF" openssl list -cipher-algorithms 2>/dev/null | grep '@ ish' || true)
if [ -n "$offered" ]; then
    printf '%s\n' "$offered" | sed 's/^ */  /'
else
    say "  (none -- the provider loaded but declined every algorithm)"
    say "  Check that 'Enable Crypto Accel' is on in iSH-AOK Settings."
fi

# A speed delta is the other half of the proof. Note that openssl speed times
# with guest CPU-time accounting, which is unreliable under emulation: treat a
# small or noisy delta as inconclusive and trust the list above.
BASE_CNF="$WORK/base.cnf"
strip_config "$CNF" "$BASE_CNF"
for alg in chacha20 aes-256-gcm; do
    printf '%s\n' "$offered" | grep -qi "${alg%%-*}" || continue
    before=$(OPENSSL_CONF="$BASE_CNF" openssl speed -evp "$alg" -seconds 1 2>/dev/null | awk 'NF>2 && $1 !~ /^(type|The|version|built|options|CPUINFO)/ {print $NF}' | tail -1)
    after=$(OPENSSL_CONF="$CNF"       openssl speed -evp "$alg" -seconds 1 2>/dev/null | awk 'NF>2 && $1 !~ /^(type|The|version|built|options|CPUINFO)/ {print $NF}' | tail -1)
    if [ -n "$before" ] && [ -n "$after" ]; then
        say ""
        say "$alg at 16 KiB blocks:"
        say "  without accelerator: $before"
        say "  with accelerator:    $after"
    fi
done

say ""
say "Done. Programs pick this up when they next start, so restart long-running"
say "ones (sshd, for instance) to accelerate new connections."
