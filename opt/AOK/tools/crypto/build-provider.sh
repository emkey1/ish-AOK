#!/bin/sh
# Build the iSH OpenSSL crypto-accelerator provider in-guest and install it so
# ssh/scp/openssl transparently use the host-native ChaCha20 accelerator.
#
# Requires: a C compiler and libcrypto.so.3 in the guest, plus the OpenSSL
# provider development headers (openssl/core.h etc). If your rootfs lacks them,
# install the -dev package (Alpine: apk add openssl-dev; Debian: apt install
# libssl-dev) or point -I at a vendored copy.
set -e
SRC="$(dirname "$0")/ish_provider.c"
DEST="${1:-/usr/lib/ossl-modules}"
CNF="${OPENSSL_CNF:-/etc/ssl/openssl.cnf}"
mkdir -p "$DEST"
cc -O2 -fPIC -shared -o "$DEST/ish.so" "$SRC"
echo "installed provider: $DEST/ish.so"
echo "add to $CNF (or set OPENSSL_CONF) to enable transparently:"
sed "s#/realmnt/ish.so#$DEST/ish.so#" "$(dirname "$0")/ish-openssl.cnf.template"
