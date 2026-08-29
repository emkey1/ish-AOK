#!/bin/sh
#
# Xcode Cloud runs this automatically after cloning, before any build action.
# Nothing else in the repo installs build dependencies, so without it a cloud
# build reaches the "Meson" legacy target with an empty toolchain and
# xcode-meson.sh exits at its first check ("meson not found in PATH").
#
# Mirrors the macOS job in .github/workflows/ci.yml, which is green. Keep the
# two in step: a dependency added there is needed here too.
#
# Structure matters as much as content. NEITHER a legacy target's stderr NOR
# this script's output reaches the Xcode Cloud build summary -- the summary
# says only "Running ci_post_clone.sh script failed (exited with code 1)". So
# this script does not abort at the first problem. It records every failure,
# keeps going, and ends with one report naming everything that is missing, so
# a single reading of the full log explains the whole build rather than its
# first mishap.

set -x

cd "${CI_PRIMARY_REPOSITORY_PATH:-$(dirname "$0")/..}" || exit 1

# xcode-meson.sh and xcode-ninja.sh bootstrap these themselves, but brew and
# the checks below run first. On Xcode Cloud brew is not always already here.
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin"
export PATH
# Auto-update turns a 30 second install into minutes and can fail on its own.
HOMEBREW_NO_AUTO_UPDATE=1
HOMEBREW_NO_INSTALL_CLEANUP=1
HOMEBREW_NO_ANALYTICS=1
export HOMEBREW_NO_AUTO_UPDATE HOMEBREW_NO_INSTALL_CLEANUP HOMEBREW_NO_ANALYTICS

problems=""
note_problem() { problems="$problems
  - $1"; }

# --- submodules ---------------------------------------------------------
# Non-recursive, matching the green Actions job. deps/linux is "update = none"
# in .gitmodules and stays unfetched. A no-op if Xcode Cloud already did it.
# Every submodule URL is a public GitHub repo, verified, so this needs no
# credentials.
git submodule update --init || note_problem "git submodule update --init failed"

# --- Homebrew packages --------------------------------------------------
# meson and ninja must come from brew rather than pip: both legacy targets put
# only Homebrew's and /usr/local's bin on PATH and look nowhere else, so a pip
# install into a user directory would be invisible to them.
# llvm is for llvm-objcopy, which Xcode does not ship and meson.build probes at
# the Homebrew keg path. lld and libarchive are parity with the Actions job and
# are not referenced by meson.build, so they are best effort.
if command -v brew >/dev/null 2>&1; then
    for formula in meson ninja llvm; do
        brew install "$formula" || note_problem "brew install $formula failed"
    done
    for formula in lld libarchive; do
        brew install "$formula" || echo "note: optional formula $formula did not install"
    done
else
    note_problem "brew is not on PATH, so no dependency could be installed"
fi

# --- Rust ---------------------------------------------------------------
# app/iSH.xcconfig sets AOK_NATIVE_HELIX=YES, so meson gets
# -Dnative_helix=enabled, which meson.build treats as a hard error without
# cargo and llvm-objcopy rather than as a silent downgrade.
#
# rustup is required even when the image already ships cargo: the iOS std has
# to be added, only rustup can add it, and a cargo without it builds the crate
# for the host, whose objects will not link into an iOS binary.
rustup_bin=""
if [ -x "$HOME/.cargo/bin/rustup" ]; then
    rustup_bin="$HOME/.cargo/bin/rustup"
elif command -v rustup >/dev/null 2>&1; then
    rustup_bin=$(command -v rustup)
fi

if [ -z "$rustup_bin" ] && command -v brew >/dev/null 2>&1; then
    # Homebrew's rustup formula is keg-only, so rustup-init is not on PATH.
    brew install rustup || note_problem "brew install rustup failed"
    rustup_init=""
    for candidate in "$(brew --prefix rustup 2>/dev/null)/bin/rustup-init" "$(command -v rustup-init 2>/dev/null)"; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            rustup_init="$candidate"
            break
        fi
    done
    if [ -n "$rustup_init" ]; then
        "$rustup_init" -y --no-modify-path --default-toolchain stable \
            || note_problem "rustup-init failed"
        [ -x "$HOME/.cargo/bin/rustup" ] && rustup_bin="$HOME/.cargo/bin/rustup"
    else
        note_problem "rustup-init not found after installing the rustup formula"
    fi
fi

if [ -n "$rustup_bin" ]; then
    # The Archive action builds for the device only.
    "$rustup_bin" target add aarch64-apple-ios \
        || note_problem "rustup target add aarch64-apple-ios failed"
else
    note_problem "no rustup, so the aarch64-apple-ios std cannot be installed"
fi

# --- report -------------------------------------------------------------
set +x
echo "=============== ci_post_clone toolchain report ==============="
# cargo is found by meson at -Dcargo_home/bin, not necessarily on PATH.
for tool in meson ninja python3 cargo rustup llvm-objcopy; do
    path=$(command -v "$tool" 2>/dev/null || true)
    if [ -z "$path" ] && [ -x "$HOME/.cargo/bin/$tool" ]; then
        path="$HOME/.cargo/bin/$tool"
    fi
    if [ -n "$path" ]; then
        echo "  $tool: $path ($("$path" --version 2>&1 | head -1))"
    else
        echo "  $tool: NOT FOUND"
    fi
done
# meson.build probes these two keg paths directly rather than PATH, so a
# missing llvm-objcopy above is not necessarily a problem.
for probe in /opt/homebrew/opt/llvm/bin/llvm-objcopy /usr/local/opt/llvm/bin/llvm-objcopy; do
    [ -x "$probe" ] && echo "  llvm-objcopy (meson probe path): $probe"
done
echo "  deps/helix: $([ -d deps/helix/.git ] || [ -f deps/helix/.git ] && echo checked-out || echo EMPTY)"
echo "  PATH=$PATH"

# Only these actually stop a build. Everything else above is informational.
missing=""
command -v meson  >/dev/null 2>&1 || missing="$missing meson"
command -v ninja  >/dev/null 2>&1 || missing="$missing ninja"
[ -x "$HOME/.cargo/bin/cargo" ] || command -v cargo >/dev/null 2>&1 || missing="$missing cargo"
[ -x /opt/homebrew/opt/llvm/bin/llvm-objcopy ] \
    || [ -x /usr/local/opt/llvm/bin/llvm-objcopy ] \
    || command -v llvm-objcopy >/dev/null 2>&1 \
    || missing="$missing llvm-objcopy"

if [ -n "$problems" ]; then
    echo "  steps that reported a problem:$problems"
fi

if [ -n "$missing" ]; then
    echo "ERROR: the build cannot proceed without:$missing"
    echo "=============== end report ==============="
    exit 1
fi

echo "  all required tools present"
echo "=============== end report ==============="
