#!/bin/sh
#
# Xcode Cloud runs this automatically after cloning, before any build action.
# Nothing else in the repo installs build dependencies, so without this file a
# cloud build reaches the "Meson" legacy target with an empty toolchain and
# xcode-meson.sh exits at its first check ("meson not found in PATH"). Xcode
# surfaces that only as "Command ExternalBuildToolExecution failed with a
# nonzero exit code", plus an "Internal inconsistency error: never received
# target ended message" from the build system noticing the target died, so the
# actual cause never reaches the build summary.
#
# This mirrors the macOS job in .github/workflows/ci.yml, which is green. Keep
# the two in step: if a dependency is added there it is needed here as well.

set -ex

cd "${CI_PRIMARY_REPOSITORY_PATH:-$(dirname "$0")/..}"

# Whatever happens below, end the log with what the toolchain actually looks
# like. A failure that names the missing tool costs one cloud build; a failure
# that does not costs a round trip to find out.
report_toolchain() {
    status=$?
    set +x
    echo "--- toolchain after ci_post_clone (exit $status) ---"
    for tool in meson ninja python3 cargo rustup llvm-objcopy; do
        path=$(command -v "$tool" 2>/dev/null || true)
        [ -z "$path" ] && [ -x "$HOME/.cargo/bin/$tool" ] && path="$HOME/.cargo/bin/$tool"
        if [ -n "$path" ]; then
            echo "$tool: $path ($("$path" --version 2>&1 | head -1))"
        else
            echo "$tool: NOT FOUND"
        fi
    done
    # meson.build probes these two keg paths directly rather than PATH.
    for probe in /opt/homebrew/opt/llvm/bin/llvm-objcopy /usr/local/opt/llvm/bin/llvm-objcopy; do
        [ -x "$probe" ] && echo "llvm-objcopy (meson probe path): $probe"
    done
    echo "PATH=$PATH"
    echo "--- end toolchain ---"
    return $status
}
trap report_toolchain EXIT

# xcode-meson.sh and xcode-ninja.sh both add these to PATH themselves, but the
# checks at the end of this script run before either of them does.
PATH="$PATH:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin"
export PATH

# Non-recursive, matching the green Actions job. deps/linux is "update = none"
# in .gitmodules and stays unfetched. A no-op if Xcode Cloud already cloned them.
git submodule update --init

# meson and ninja land in Homebrew's bin, which is exactly what the two legacy
# targets bootstrap onto PATH. A pip install would land somewhere they do not
# look, so it has to be brew here even though the Actions job uses pip.
# llvm is for llvm-objcopy, which Xcode does not ship and which meson.build
# probes at the Homebrew keg path. lld and libarchive are for parity with the
# Actions job.
brew install meson ninja llvm lld libarchive

# app/iSH.xcconfig sets AOK_NATIVE_HELIX=YES, so meson gets
# -Dnative_helix=enabled, which is a hard error without cargo and llvm-objcopy
# rather than a silent downgrade. The iOS target matters as much as the
# toolchain: cargo alone would build the crate for the host and the objects
# would not link.
if ! command -v cargo >/dev/null 2>&1 && [ ! -x "$HOME/.cargo/bin/cargo" ]; then
    # Homebrew's rustup formula is keg-only, so rustup-init is not on PATH.
    brew install rustup
    "$(brew --prefix rustup)/bin/rustup-init" -y --no-modify-path --default-toolchain stable
fi

rustup_bin=""
if [ -x "$HOME/.cargo/bin/rustup" ]; then
    rustup_bin="$HOME/.cargo/bin/rustup"
elif command -v rustup >/dev/null 2>&1; then
    rustup_bin=$(command -v rustup)
fi

if [ -n "$rustup_bin" ]; then
    # The Archive action builds for the device only.
    "$rustup_bin" target add aarch64-apple-ios
else
    echo "error: no rustup, so the aarch64-apple-ios std cannot be installed." >&2
    echo "AOK_NATIVE_HELIX=YES makes that a hard meson error, not a downgrade." >&2
    exit 1
fi

# Fail here, with a name, rather than inside a legacy target whose stderr does
# not reach the build summary.
for tool in meson ninja; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: $tool is still not on PATH after the bootstrap." >&2
        exit 1
    }
done
