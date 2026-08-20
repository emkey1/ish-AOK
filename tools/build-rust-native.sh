#!/bin/sh
# Build the Rust native program and rewrite its libc imports onto the shim.
#
# Two steps that have to happen together, which is why they are one script
# rather than two meson rules: cargo produces an archive whose open()/read()
# bind to the HOST, and it is only safe to link once those are renamed. A
# half-applied version of this is an archive that silently reads iOS's
# filesystem, so the rename is not an optimisation to skip on a rebuild.
set -eu

crate_dir=$1; out=$2; cargo=$3; objcopy=$4; renames=$5; target=${6:-}

cargo_args="--release --quiet --manifest-path $crate_dir/Cargo.toml"
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

# The rename list is generated from kernel/native_libc.h at build time, so it
# cannot fall behind the header. See tools/gen-nlibc-renames.py.
# shellcheck disable=SC2046
"$objcopy" $(tr '\n' ' ' < "$renames") "$staticlib" "$out"

# A rewritten archive that still imports a raw libc name is the failure this
# whole mechanism exists to prevent, and it is SILENT: the link succeeds and
# the guest reads the host's files. Worse, llvm-objcopy does not always manage
# the rewrite -- it leaves undefined symbols in some Mach-O members untouched
# and reports success -- so this check is not belt and braces, it is the only
# thing standing between a missed rename and a wrong answer.
#
# Checked against the whole generated list rather than a hand-picked few.
raw=$(nm -u "$out" 2>/dev/null | sed 's/^ *U //' | sort -u)
missed=""
while IFS= read -r line; do
    case $line in
        _*=_*) from=${line%%=*} ;;
        *) continue ;;
    esac
    # sysctlbyname is the one name allowed to survive, and only because the
    # split in nlibc_sysctlbyname makes both paths agree -- see the comment
    # there and in tools/check-native-libc.py. Every other survivor is a bug.
    if [ "$from" = "_sysctlbyname" ]; then
        continue
    fi
    if printf '%s\n' "$raw" | grep -qx -- "$from"; then
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
