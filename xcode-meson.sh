#!/bin/bash

set -euo pipefail

bootstrap_path() {
    local extra
    for extra in /opt/homebrew/bin /opt/homebrew/sbin /usr/local/bin /usr/local/sbin; do
        case ":$PATH:" in
            *":$extra:"*) ;;
            *) PATH="$PATH:$extra" ;;
        esac
    done
    export PATH
}

bootstrap_path

if ! command -v meson >/dev/null 2>&1; then
    echo "meson not found in PATH: $PATH" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not found in PATH: $PATH" >&2
    exit 1
fi

declare -a arch_list=()
if [[ -n "${ARCHS:-}" ]]; then
    for arch in $ARCHS; do
        arch_list+=("$arch")
    done
else
    arch_list+=("${CURRENT_ARCH:-$(uname -m)}")
fi

mkdir -p "$MESON_BUILD_DIR"

meson_cpu_family() {
    local arch="$1"
    case "$arch" in
        arm64) echo aarch64 ;;
        *) echo "$arch" ;;
    esac
}

apple_sdk_name() {
    local sdk="${SDKROOT:-}"
    if [[ -z "$sdk" ]]; then
        case "${PLATFORM_NAME:-}" in
            iphoneos|iphonesimulator|macosx) sdk="$PLATFORM_NAME" ;;
            *) sdk=macosx ;;
        esac
    elif [[ "$sdk" == */* ]]; then
        case "$sdk" in
            *iPhoneOS.platform/*) sdk=iphoneos ;;
            *iPhoneSimulator.platform/*) sdk=iphonesimulator ;;
            *MacOSX.platform/*) sdk=macosx ;;
        esac
    fi
    echo "$sdk"
}

apple_sdk_path() {
    local sdk_name="$1"
    if [[ -n "${SDKROOT:-}" && "${SDKROOT:-}" == */* && -d "${SDKROOT:-}" ]]; then
        echo "$SDKROOT"
    else
        xcrun --sdk "$sdk_name" --show-sdk-path
    fi
}

apple_target_triple() {
    local sdk_name="$1"
    local arch="$2"
    case "$sdk_name" in
        iphoneos)
            local version="${IPHONEOS_DEPLOYMENT_TARGET:-15.0}"
            echo "${arch}-apple-ios${version}"
            ;;
        iphonesimulator)
            local version="${IPHONEOS_DEPLOYMENT_TARGET:-15.0}"
            echo "${arch}-apple-ios${version}-simulator"
            ;;
        macosx)
            local version="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
            echo "${arch}-apple-macos${version}"
            ;;
        *)
            echo "${arch}-apple-darwin"
            ;;
    esac
}

json_array() {
    python3 - "$@" <<'PY'
import json
import sys

print(json.dumps(sys.argv[1:]))
PY
}

meson_option_json() {
    local config_json="$1"
    local option_name="$2"
    OPTION_NAME="$option_name" python3 -c 'import json, os, sys
name = os.environ["OPTION_NAME"]
for option in json.load(sys.stdin):
    if option["name"] == name:
        print(json.dumps(option["value"]))
        break
else:
    raise SystemExit(f"missing option: {name}")' <<<"$config_json"
}

configure_arch() {
    local arch="$1"
    local meson_dir="$MESON_BUILD_DIR/$arch"
    local crossfile_dir="$MESON_BUILD_DIR/cross"
    local crossfile="$crossfile_dir/$arch.txt"
    local crossfile_tmp="$crossfile_dir/$arch.txt.tmp"
    local meson_arch
    local config
    local sdk_name
    local sdk_path
    local target_triple
    local meson_needs_setup=0
    local meson_needs_wipe=0
    local desired_c_args_json
    local current_c_args_json
    local current_c_link_args_json

    mkdir -p "$meson_dir" "$crossfile_dir"

    sdk_name=$(apple_sdk_name)
    sdk_path=$(apple_sdk_path "$sdk_name")
    target_triple=$(apple_target_triple "$sdk_name" "$arch")
    meson_arch=$(meson_cpu_family "$arch")
    desired_c_args_json=$(json_array -target "$target_triple" -isysroot "$sdk_path")

    cat >"$crossfile_tmp" <<-EOF
	[binaries]
	c = 'clang'
	objc = 'clang'
	ar = 'ar'

	[host_machine]
	system = 'darwin'
	cpu_family = '$meson_arch'
	cpu = '$meson_arch'
	endian = 'little'

	[built-in options]
	c_args = ['-target', '$target_triple', '-isysroot', '$sdk_path']
	c_link_args = ['-target', '$target_triple', '-isysroot', '$sdk_path']
	objc_args = ['-target', '$target_triple', '-isysroot', '$sdk_path']
	objc_link_args = ['-target', '$target_triple', '-isysroot', '$sdk_path']

	[properties]
	needs_exe_wrapper = true
EOF

    # The Rust native program needs two things Xcode's environment does not
    # supply: where rustup put cargo (it is not on PATH under xcodebuild), and
    # the Rust target triple, which must be the iOS one rather than the host's
    # -- a crate built for macOS links fine and targets the wrong platform.
    # Both are empty-safe: without cargo the crate is simply not built, and
    # everything else builds exactly as before.
    local rust_triple=""
    case "$sdk_name" in
        iphoneos)          rust_triple="aarch64-apple-ios" ;;
        iphonesimulator)   [[ "$arch" == arm64 ]] && rust_triple="aarch64-apple-ios-sim" ;;
        macosx)            [[ "$arch" == arm64 ]] && rust_triple="aarch64-apple-darwin" ;;
    esac
    local meson_extra_opts="-Dcargo_home=$HOME/.cargo"
    [[ -n "$rust_triple" ]] && meson_extra_opts="$meson_extra_opts -Dnative_rust_target=$rust_triple"
    # AOK_RUST_FEATURES comes from app/iSH.xcconfig, so the Rust program's
    # cargo features are set where every other project-wide knob is set rather
    # than on a command line. Empty for a normal build.
    meson_extra_opts="$meson_extra_opts -Dnative_rust_features=${AOK_RUST_FEATURES:-}"
    # AOK_NATIVE_HELIX likewise comes from app/iSH.xcconfig. Without this the
    # meson default (disabled) won a build that had asked for an editor, and
    # the only symptom was /AOK/native/hx not being there.
    if [[ "${AOK_NATIVE_HELIX:-}" == YES ]]; then
        meson_extra_opts="$meson_extra_opts -Dnative_helix=enabled"
    else
        meson_extra_opts="$meson_extra_opts -Dnative_helix=disabled"
    fi

    if [[ ! -f "$crossfile" ]] || ! cmp -s "$crossfile_tmp" "$crossfile"; then
        mv "$crossfile_tmp" "$crossfile"
        meson_needs_setup=1
    else
        rm -f "$crossfile_tmp"
    fi

    export CC_FOR_BUILD="env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun clang"
    export CC="$CC_FOR_BUILD" # compatibility with meson < 0.54.0

    if [[ ! -f "$meson_dir/meson-private/coredata.dat" ]]; then
        meson_needs_setup=1
    else
        config=$(meson introspect --buildoptions "$meson_dir")
        current_c_args_json=$(meson_option_json "$config" c_args)
        current_c_link_args_json=$(meson_option_json "$config" c_link_args)
        if [[ "$current_c_args_json" != "$desired_c_args_json" ]] || [[ "$current_c_link_args_json" != "$desired_c_args_json" ]]; then
            meson_needs_wipe=1
        fi
        # Options are read at setup time only, so a build directory made
        # before one of these existed keeps its old answer and quietly leaves
        # the Rust native program out.
        #
        # Teaching a build directory an option it has never heard of, and
        # setting that option, are TWO steps and cannot be one. `meson setup
        # --reconfigure` validates -D against the options the directory
        # already knows, so passing -D for a new one is rejected outright --
        # "ERROR: Unknown option" -- however current meson_options.txt is.
        # A plain --reconfigure, with no -D at all, re-reads the option
        # definitions and adds the newcomer at its default; only then will the
        # -D be accepted. The declared-option loop further down has used that
        # same trick since guest_archs, and this block ignored it and broke
        # every Xcode build against an existing DerivedData tree.
        #
        # So: teach first, re-read, and let the ordinary value comparison
        # below decide whether anything still has to be set. Anyone adding a
        # fourth option here inherits the rule.
        read_rust_opts() {
            current_cargo_home=$(meson_option_json "$config" cargo_home 2>/dev/null || echo MISSING)
            current_rust_target=$(meson_option_json "$config" native_rust_target 2>/dev/null || echo MISSING)
            current_rust_features=$(meson_option_json "$config" native_rust_features 2>/dev/null || echo MISSING)
        current_helix=$(meson_option_json "$config" native_helix 2>/dev/null || echo MISSING)
        want_helix=$([[ "${AOK_NATIVE_HELIX:-}" == YES ]] && echo '"enabled"' || echo '"disabled"')
        }
        read_rust_opts
        if [[ "$current_cargo_home" == MISSING ]] || \
           [[ "$current_rust_target" == MISSING ]] || \
           [[ "$current_rust_features" == MISSING ]] || \
           [[ "$current_helix" == MISSING ]]; then
            (set -x; meson setup --reconfigure "$meson_dir" "$SRCROOT" --cross-file "$crossfile") || exit $?
            config=$(meson introspect --buildoptions "$meson_dir")
            read_rust_opts
        fi
        if [[ "$current_cargo_home" != "\"$HOME/.cargo\"" ]] || \
           [[ "$current_rust_target" != "\"$rust_triple\"" ]] || \
           [[ "$current_rust_features" != "\"${AOK_RUST_FEATURES:-}\"" ]] || \
           [[ "$current_helix" != "$want_helix" ]]; then
            meson_needs_setup=1
        fi
    fi

    if (( meson_needs_wipe )); then
        (set -x; meson setup --wipe "$meson_dir" "$SRCROOT" --cross-file "$crossfile" $meson_extra_opts) || exit $?
    elif [[ ! -f "$meson_dir/meson-private/coredata.dat" ]]; then
        (set -x; meson setup "$meson_dir" "$SRCROOT" --cross-file "$crossfile" $meson_extra_opts) || exit $?
    elif (( meson_needs_setup )); then
        (set -x; meson setup --reconfigure "$meson_dir" "$SRCROOT" --cross-file "$crossfile" $meson_extra_opts) || exit $?
    fi

    cd "$meson_dir"
    config=$(meson introspect --buildoptions)

    # Optimized in BOTH Xcode configurations. This used to follow
    # $CONFIGURATION, so a Debug build compiled the emulator core at -O0, and
    # an -O0 emulator is not "a bit slower" -- it silently invalidates any
    # measurement taken on it. It has cost us twice now: amd64 gzip ran 2.1x
    # slower than at -O2 and reordered the whole cross-arch benchmark ranking,
    # and later a full round of crypto-accelerator benchmarking concluded the
    # accelerator was a net loss when the real finding was that -O0 cost it
    # 14x (AES-256-GCM: 53.6 MB/s at -O0 against 770.8 at -O2, on identical
    # hardware and source).
    #
    # debugoptimized is -O2 *with* debug info, and b_ndebug stays false so
    # assertions remain live, so this keeps almost all of the debugging value.
    buildtype=debugoptimized
    b_ndebug=false
    # Opt in to an unoptimized build when you genuinely need one -- single
    # stepping the emulator core, or chasing something optimizer-dependent:
    #   ISH_MESON_BUILDTYPE=debug   (as a build setting in iSH.xcconfig)
    # ⚠ It must be a BUILD SETTING, not a scheme "Environment Variable". Those
    # live under the scheme's <LaunchAction> and are handed to the app when it is
    # LAUNCHED; a build script phase such as this one never sees them, so the
    # setting silently does nothing and the build looks like it succeeded. This
    # is not hypothetical: it cost a real on-device A/B, which measured a flat
    # result that was actually one configuration compared against itself.
    # Build settings from an xcconfig ARE exported into script phases, which is
    # why that is the working place to put these.
    # uname -v reports " unoptimized" whenever this is in effect, so a build
    # that is slow for this reason says so rather than being mistaken for data.
    if [[ -n "${ISH_MESON_BUILDTYPE:-}" ]]; then
        buildtype=$ISH_MESON_BUILDTYPE
    fi
    b_sanitize=none
    if [[ -n "${ENABLE_ADDRESS_SANITIZER:-}" ]]; then
        b_sanitize=address
    fi
    log=${ISH_LOG:-}
    log_handler=${ISH_LOGGER:-}
    kernel=ish
    if [[ -n "${ISH_KERNEL:-}" ]]; then
        kernel=$ISH_KERNEL
    fi
    kconfig=""
    # Guest architectures the app build supports; comma-separated subset of
    # i386,amd64,arm64. Set ISH_GUEST_ARCHS in iSH.xcconfig (or the scheme
    # environment) to trim the emulator; meson rejects an empty list.
    guest_archs=${ISH_GUEST_ARCHS:-i386,amd64,arm64,riscv64}
    # arm64-guest gadget dispatch: 'ldar' (default, fastest on Apple Silicon) or
    # 'dmb' (ldr + dmb ishld, expected better on ARMv8.0 devices like the A9).
    # Set ISH_ARM64_GRET=dmb as a build setting in iSH.xcconfig (NOT a scheme
    # environment variable -- see the warning above) to build
    # the variant for an on-device A/B; see jit/guest-arm64/gadgets.h for why
    # this is host-generation dependent and why it must be measured on ARMv8.0.
    arm64_gret=${ISH_ARM64_GRET:-dmb}
    # Options added to meson_options.txt after this build dir was set up are
    # invisible to `meson configure`, which only knows what the dir was
    # configured with -- so a NEW option silently keeps whatever default was
    # current when the dir was created, and a changed default never arrives.
    # native_zsh was added and defaulted on, and an existing DerivedData tree
    # kept building without zsh and said nothing.
    #
    # The per-variable fallback below catches this only for the names it
    # iterates. This catches it for every option the project declares.
    declared=$(grep -oE "^option\('[a-z0-9_]+'" "$SRCROOT/meson_options.txt" | sed "s/option('//;s/'//")
    for opt in $declared; do
        if ! grep -q "\"name\": \"$opt\"" <<< "$config"; then
            (set -x; meson setup --reconfigure . "$SRCROOT" --cross-file "$crossfile") || exit $?
            config=$(meson introspect --buildoptions)
            break
        fi
    done

    for var in buildtype log b_ndebug b_sanitize log_handler kernel kconfig guest_archs arm64_gret; do
        if ! old_value=$(python3 -c "import sys, json; v = next(x['value'] for x in json.load(sys.stdin) if x['name'] == '$var'); print(str(v).lower() if isinstance(v, bool) else ','.join(v) if isinstance(v, list) else v)" <<< "$config" 2>/dev/null); then
            # The option is missing from this build dir's cached
            # configuration: it was added to meson_options.txt after the
            # dir was set up (e.g. guest_archs on an existing DerivedData
            # tree). A plain `meson configure` can't learn new options —
            # --reconfigure re-reads the option definitions. Without this,
            # the next(...) one-liner died with StopIteration and, under
            # `set -e`, took the whole Xcode build with it.
            (set -x; meson setup --reconfigure . "$SRCROOT" --cross-file "$crossfile") || exit $?
            config=$(meson introspect --buildoptions)
            old_value=$(python3 -c "import sys, json; v = next(x['value'] for x in json.load(sys.stdin) if x['name'] == '$var'); print(str(v).lower() if isinstance(v, bool) else ','.join(v) if isinstance(v, list) else v)" <<< "$config")
        fi
        new_value=${!var}
        if [[ $old_value != $new_value ]]; then
            set -x; meson configure "-D$var=$new_value"
        fi
    done
}

# hterm's shipped bundle is a build artifact, not a source file.
#
# app/terminal/term.html loads deps/libapps/hterm/dist/js/hterm_all.js, and
# hterm/dist is gitignored -- so the bundle only exists because somebody ran
# bin/mkdist at some point, and an edit to hterm's SOURCES silently shipped
# whatever bundle was already there. A line-height preference added to
# hterm/js took three builds to appear for exactly that reason, and a fresh
# clone has no bundle at all.
#
# Done here rather than in the "Compile JavaScript" phase because that is a
# user script phase, and ENABLE_USER_SCRIPT_SANDBOXING (on, deliberately)
# denies it even reading bin/mkdist. This script is a legacy target's build
# tool, which is not sandboxed, and it already runs before everything else.
regenerate_hterm_bundle() {
    local hterm="$SRCROOT/deps/libapps/hterm"
    local dist="$hterm/dist/js/hterm_all.js"
    [ -d "$hterm/js" ] || return 0        # submodule not checked out; not our problem
    local stale=""
    if [ ! -f "$dist" ]; then
        stale="bundle missing"
    else
        stale=$(find "$hterm/js" "$SRCROOT/deps/libapps/libdot/js" -name '*.js' -newer "$dist" -print -quit 2>/dev/null || true)
    fi
    [ -n "$stale" ] || return 0
    echo "note: regenerating hterm_all.js ($stale)"
    (cd "$hterm" && python3 bin/mkdist >/dev/null) || {
        echo "error: hterm/bin/mkdist failed -- hterm_all.js would be stale" >&2
        return 1
    }
}
regenerate_hterm_bundle

for arch in "${arch_list[@]}"; do
    configure_arch "$arch"
done
