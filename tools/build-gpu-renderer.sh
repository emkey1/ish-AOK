#!/bin/sh
# Builds the renderer behind /dev/dri/renderD128 (fs/virtgpu.c, #484):
# virglrenderer's Venus renderer, from deps/virglrenderer (emkey1's fork,
# branch ish-aok), for the Mac CLI and for iOS devices, plus MoltenVK's iOS
# static library. Prints the settings that turn the node on; without them
# there is no node.
#
#   sh tools/build-gpu-renderer.sh [OUT]        (default ~/.cache/aok-gpu)
#
# Needs python3, gh, and Homebrew's ninja, pkg-config, vulkan-headers and (to
# run the CLI) molten-vk and vulkan-loader. No network beyond MoltenVK: the
# fork carries venus-protocol.
set -eu
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$HOME/.cache/aok-gpu}
MOLTENVK=v1.4.2
BREW=$(brew --prefix)

mkdir -p "$OUT"
cd "$OUT"
[ -x venv/bin/meson ] || { python3 -m venv venv && venv/bin/pip -q install mako meson; }
PATH=$OUT/venv/bin:$PATH

V="$SRC/deps/virglrenderer"
[ -f "$V/src/venus/vkr_renderer.c" ] || { echo "deps/virglrenderer is not checked out: git submodule update --init deps/virglrenderer" >&2; exit 1; }
cd "$V"

common="-Dvrend=false -Dvenus=true -Drender-server-worker=thread -Dtests=false"
# The Mac CLI: the renderer dlopens the Vulkan loader, so run ish with
# DYLD_FALLBACK_LIBRARY_PATH=$BREW/lib.
[ -d "$OUT/build-mac" ] || meson setup --wrap-mode=nodownload "$OUT/build-mac" $common
ninja -C "$OUT/build-mac" src/libvirgl.a src/mesa/libmesa.a

# iOS: static, and linked against MoltenVK rather than dlopening anything.
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
mkdir -p "$OUT/pc-ios"
printf 'Name: vulkan\nDescription: Vulkan headers (the app links MoltenVK)\nVersion: 1.4\nCflags: -I%s/include\nLibs:\n' \
    "$BREW" > "$OUT/pc-ios/vulkan.pc"
cat > "$OUT/ios-arm64.cross" <<CROSS
[binaries]
c = ['xcrun', '--sdk', 'iphoneos', 'clang']
objc = ['xcrun', '--sdk', 'iphoneos', 'clang']
cpp = ['xcrun', '--sdk', 'iphoneos', 'clang++']
ar = ['xcrun', '--sdk', 'iphoneos', 'ar']
strip = ['xcrun', '--sdk', 'iphoneos', 'strip']
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=15.0']
objc_args = ['-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=15.0']
c_link_args = ['-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=15.0']
objc_link_args = ['-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=15.0']

[properties]
pkg_config_libdir = '$OUT/pc-ios'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
CROSS
[ -d "$OUT/build-ios" ] || meson setup --wrap-mode=nodownload "$OUT/build-ios" \
    --cross-file "$OUT/ios-arm64.cross" -Ddefault_library=static -Dvulkan-dload=false $common
ninja -C "$OUT/build-ios" src/libvirgl.a src/mesa/libmesa.a

cd "$OUT"
if [ ! -f moltenvk-ios/MoltenVK/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a ]; then
    mkdir -p moltenvk-ios
    gh release download "$MOLTENVK" -R KhronosGroup/MoltenVK -p MoltenVK-ios.tar -D moltenvk-ios --clobber
    tar xf moltenvk-ios/MoltenVK-ios.tar -C moltenvk-ios
fi

M="$OUT/moltenvk-ios/MoltenVK/MoltenVK/static/MoltenVK.xcframework/ios-arm64"
cat <<DONE

CLI:  meson configure build -Dvirglrenderer=$OUT/build-mac
      (run: DYLD_FALLBACK_LIBRARY_PATH=$BREW/lib ./build/ish ...)
App (iOS device), as xcodebuild settings or in app/iSH.xcconfig:
  AOK_VIRGLRENDERER_DIR=$OUT/build-ios
  AOK_VIRGL_LDFLAGS=$OUT/build-ios/src/libvirgl.a $OUT/build-ios/src/mesa/libmesa.a $M/libMoltenVK.a -lc++ -framework Metal -framework IOSurface -framework QuartzCore
DONE
