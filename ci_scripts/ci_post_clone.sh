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
HOMEBREW_NO_ENV_HINTS=1
export HOMEBREW_NO_AUTO_UPDATE HOMEBREW_NO_INSTALL_CLEANUP HOMEBREW_NO_ANALYTICS HOMEBREW_NO_ENV_HINTS

problems=""
note_problem() { problems="$problems
  - $1"; }

# --- submodules ---------------------------------------------------------
# Non-recursive, matching the green Actions job. A no-op if Xcode Cloud
# already did it.
# Every submodule URL is a public GitHub repo, verified, so this needs no
# credentials.
git submodule update --init || note_problem "git submodule update --init failed"

# --- generated sources the checkout does not carry -----------------------
# The two steps the Actions macOS job runs between checkout and xcodebuild, and
# which this script lacked -- the dash one was added to ci.yml on 2026-09-16,
# after this script was written, and every cloud archive since failed on it at
# meson.build ("-Dnative_dash=enabled but deps/dash is not a prepared dash
# tree"), where only the ExternalBuildToolExecution failure reached the summary.
#
# deps/dash's config.h: the fork commits dash's generated sources but not
# config.h. Configure only; never dash's own make (tools/configure-dash.sh says
# why).
(cd deps/dash && ../../tools/configure-dash.sh --quiet) \
    || note_problem "configuring deps/dash failed"
# hterm_all.js: the terminal's JavaScript bundle, a build artifact of libapps
# that the app target copies in as a resource.
(cd deps/libapps/hterm && python3 bin/mkdist) \
    || note_problem "building hterm_all.js (deps/libapps/hterm bin/mkdist) failed"

# --- Homebrew packages --------------------------------------------------
# meson and ninja must come from brew rather than pip: both legacy targets put
# only Homebrew's and /usr/local's bin on PATH and look nowhere else, so a pip
# install into a user directory would be invisible to them.
# llvm is for llvm-objcopy, which Xcode does not ship and meson.build probes at
# the Homebrew keg path, and for the clang vdso/meson.build compiles the i386
# vDSO with. lld is for that vDSO's link: vdso/meson.build passes
# -fuse-ld=lld, so clang needs ld.lld on PATH, and xcode-meson.sh puts
# /opt/homebrew/bin there. lld is not keg-only, so it lands there.
#
# This script once skipped lld, on the belief that nothing referenced it. The
# build then failed at the Meson target with "clang: error: invalid linker name
# in argument '-fuse-ld=lld'" -- the exact error vdso/check-cc.sh gives
# with ld.lld off PATH. libarchive stays out: the app builds deps/libarchive
# itself, and meson.build does not ask for Homebrew's.
if command -v brew >/dev/null 2>&1; then
    for formula in meson ninja llvm lld; do
        brew install "$formula" || note_problem "brew install $formula failed"
    done
else
    note_problem "brew is not on PATH, so no dependency could be installed"
fi

# --- Metal toolchain ----------------------------------------------------
# Since Xcode 26 the Metal compiler is a separately downloaded component, not
# part of Xcode, and the Xcode Cloud image does not carry it. The app compiles
# one shader (app/DisplayRFBShaders.metal), so without it the archive fails at
# "Command CompileMetalFile failed with a nonzero exit code" -- which is where
# every cloud build ended once lld was in place. GitHub's macOS runners ship it,
# which is why the Actions job never needed this step.
if xcodebuild -showComponent MetalToolchain 2>/dev/null | grep -q 'Status: installed'; then
    echo "Metal toolchain already installed"
else
    xcodebuild -downloadComponent MetalToolchain \
        || note_problem "xcodebuild -downloadComponent MetalToolchain failed"
fi

# --- Rust ---------------------------------------------------------------
# app/iSH.xcconfig sets AOK_NATIVE_HELIX=YES, so meson gets
# -Dnative_helix=enabled, which meson.build treats as a hard error without
# cargo and llvm-objcopy rather than as a silent downgrade.
#
# rustup is required even when the image already ships cargo: the iOS std has
# to be added, only rustup can add it, and a cargo without it builds the crate
# for the host, whose objects will not link into an iOS binary.
#
# The toolchain goes in $HOME/.cargo, installed by rustup's own installer,
# because that is where meson looks: find_program('cargo', <cargo_home>/bin/cargo)
# with -Dcargo_home=$HOME/.cargo, and xcodebuild's PATH has neither ~/.cargo/bin
# nor a Homebrew keg in it.
#
# This used to go through Homebrew's rustup formula, and stopped working when
# the formula changed under it. 1.29 no longer ships rustup-init -- it installs
# the same binary as `rustup` -- and keeps cargo and rustc as proxies in its
# unlinked keg. That put a rustup on PATH, but installing a toolchain through it
# never puts cargo in ~/.cargo/bin. Every cloud build ended "the build cannot
# proceed without: cargo". Homebrew bought nothing here anyway: the toolchain
# itself is downloaded from static.rust-lang.org whichever rustup asks for it.
rustup_bin="$HOME/.cargo/bin/rustup"
if [ ! -x "$rustup_bin" ]; then
    rustup_init=$(mktemp "${TMPDIR:-/tmp}/rustup-init.XXXXXX")
    if curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o "$rustup_init"; then
        sh "$rustup_init" -y --no-modify-path --profile minimal --default-toolchain stable \
            || note_problem "the rustup installer (sh.rustup.rs) failed"
    else
        note_problem "could not download the rustup installer from sh.rustup.rs"
    fi
    rm -f "$rustup_init"
fi

if [ -x "$rustup_bin" ]; then
    # A rustup that was already here may have no toolchain of its own yet.
    "$rustup_bin" default >/dev/null 2>&1 || "$rustup_bin" default stable \
        || note_problem "rustup default stable failed"
    # The Archive action builds for the device only.
    "$rustup_bin" target add aarch64-apple-ios \
        || note_problem "rustup target add aarch64-apple-ios failed"
else
    note_problem "no rustup in \$HOME/.cargo/bin, so there is no cargo for meson and no aarch64-apple-ios std"
fi

# --- report -------------------------------------------------------------
set +x
echo "=============== ci_post_clone toolchain report ==============="
# cargo is found by meson at -Dcargo_home/bin, not necessarily on PATH.
for tool in meson ninja python3 cargo rustup llvm-objcopy ld.lld; do
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
echo "  Metal toolchain: $(xcodebuild -showComponent MetalToolchain 2>/dev/null | sed -n 's/^Status: //p' | head -1)"
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
command -v ld.lld >/dev/null 2>&1 || missing="$missing ld.lld"
xcodebuild -showComponent MetalToolchain 2>/dev/null | grep -q 'Status: installed' \
    || missing="$missing MetalToolchain"
[ -f deps/dash/config.h ] || missing="$missing deps/dash/config.h"
[ -f deps/libapps/hterm/dist/js/hterm_all.js ] || missing="$missing hterm_all.js"

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
