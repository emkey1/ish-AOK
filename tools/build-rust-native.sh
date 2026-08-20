#!/bin/sh
# Build the Rust native program and rewrite its libc imports onto the shim.
#
# Three steps that have to happen together, which is why they are one script
# rather than three meson rules: cargo produces an archive whose open()/read()
# bind to the HOST, and it is only safe to link once those are renamed. A
# half-applied version of this is an object that silently reads iOS's
# filesystem, so the rename is not an optimisation to skip on a rebuild.
set -eu

crate_dir=$1; out=$2; cargo=$3; objcopy=$4; renames=$5; target=${6:-}

cargo_args="--release --quiet --manifest-path $crate_dir/Cargo.toml"
# Reproducing the tokio measurement has to go through the real build -- the
# rename pass is the thing under test -- so the feature is reachable from the
# environment rather than only from a bare `cargo build`:
#     AOK_RUST_FEATURES=tokio-probe ninja -C build ish
# Changing it does not by itself make ninja rerun this; touch the crate's
# lib.rs, which is in depend_files.
[ -n "${AOK_RUST_FEATURES:-}" ] && cargo_args="$cargo_args --features $AOK_RUST_FEATURES"
built_dir="$crate_dir/target/release"
if [ -n "$target" ]; then
    cargo_args="$cargo_args --target $target"
    built_dir="$crate_dir/target/$target/release"
fi

# CARGO_TARGET_DIR is left alone deliberately: cargo's own layout is what the
# path above assumes, and overriding it in one place and not the other is how
# this breaks silently.
"$cargo" build $cargo_args

staticlib="$built_dir/librust_native_probe.a"
[ -f "$staticlib" ] || { echo "build-rust-native: cargo produced no $staticlib" >&2; exit 1; }

# Why the archive is flattened into one object before anything else:
#
#  1. It is the only form meson can hand to libish's `objects:`, and being
#     inside libish.a is what lets the Xcode build link this without knowing
#     it exists. A separate archive needs a -force_load that only meson's own
#     link line carries, so the app build fails to resolve the registry entry.
#  2. llvm-objcopy --redefine-sym does not reliably rewrite undefined symbols
#     in Mach-O *archive members* -- it skips some and still exits 0. On a
#     single flat object it rewrites all of them. Merging first turned a
#     rename that needed a documented exception into one with no survivors.
#
# It has to be `ar x` and then a partial link of the extracted members: `ld -r`
# reading the archive directly, with -all_load or -force_load, produces an
# 11MB object holding four symbols and exits 0. That output links, and every
# call in it goes nowhere.
work=$(mktemp -d "${TMPDIR:-/tmp}/rust-native.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM
abs_staticlib=$(cd "$(dirname "$staticlib")" && pwd)/$(basename "$staticlib")

dupes=$(ar t "$abs_staticlib" | sort | uniq -d)
if [ -n "$dupes" ]; then
    # ar x would silently keep only the last of each name.
    echo "build-rust-native: the archive has members with duplicate names:" >&2
    printf '    %s\n' $dupes >&2
    exit 1
fi

(cd "$work" && ar x "$abs_staticlib")
merged="$work/merged.o"
# clang, not ld: it supplies -platform_version from the triple, which bare
# `ld -r` refuses to go without. -sdk keeps the iOS build off the macOS
# sysroot; a host build takes the default.
sdk=""
clang_target=""
case "$target" in
    *-apple-ios-sim|*-apple-ios-macabi) sdk="iphonesimulator" ;;
    *-apple-ios)                        sdk="iphoneos" ;;
    *-apple-darwin)                     sdk="macosx" ;;
esac
# The deployment target too, when Xcode is the one asking. Without it clang
# defaults the partial link to iOS 7.0 and warns once per object that std was
# built for something newer -- harmless, and 400 lines of it.
[ -n "$target" ] && clang_target="-target $target${IPHONEOS_DEPLOYMENT_TARGET:-}"
# shellcheck disable=SC2086
(cd "$work" && xcrun ${sdk:+-sdk $sdk} clang $clang_target -nostdlib -Wl,-r -o "$merged" ./*.o)

# The rename list is generated from kernel/native_libc.h at build time, so it
# cannot fall behind the header. See tools/gen-nlibc-renames.py.
# shellcheck disable=SC2046
"$objcopy" $(tr '\n' ' ' < "$renames") "$merged" "$out"

# A rewritten object that still imports a raw libc name is the failure this
# whole mechanism exists to prevent, and it is SILENT: the link succeeds and
# the guest reads the host's files.
#
# Checked against the whole generated list rather than a hand-picked few, and
# with no exceptions -- since the merge above, there are none.
raw=$(nm -u "$out" 2>/dev/null | sed 's/^ *U //' | sort -u)
missed=""
while IFS= read -r line; do
    case $line in
        *--redefine-sym) continue ;;
        _*=_*) from=${line%%=*} ;;
        *) continue ;;
    esac
    if printf '%s\n' "$raw" | grep -qxF -- "$from"; then
        missed="$missed $from"
    fi
done < "$renames"

if [ -n "$missed" ]; then
    echo "build-rust-native: these libc imports survived the rename:" >&2
    for m in $missed; do echo "    $m" >&2; done
    echo "  llvm-objcopy could not rewrite them. Either the symbol is genuinely" >&2
    echo "  meant to reach the host (say so in tools/check-native-libc.py), or" >&2
    echo "  the crate must stop calling it." >&2
    exit 1
fi
