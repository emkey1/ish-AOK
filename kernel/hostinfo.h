#ifndef ISH_HOSTINFO_H
#define ISH_HOSTINFO_H

#include <stdint.h>
#include <time.h>

char *printHostInfo(void);
// Identifies the build itself, for uname -v. On iOS that is the app's version
// and build number ("1.3 (546)"), which is what tells you which build a device
// is actually running; elsewhere it is the compile timestamp. Caller frees.
char *copyBuildVersion(void);

// Appended to the build identifier when the emulator was compiled without
// optimization. Worth reporting because an unoptimized build is not "a bit
// slower": it changed the crypto accelerator's throughput by 13x and silently
// invalidated a full round of benchmarking, which read as "the accelerator is
// a net loss" when the real finding was "this is a debug build".
//
// Only meaningful in files the *emulator's* build system compiles (meson
// builds libish.a, and xcode-meson.sh selects buildtype=debug unless the Xcode
// configuration is Release). Do not use it from app-target sources: those are
// compiled by Xcode with its own flags and would report a different answer.
#ifdef __OPTIMIZE__
#define ISH_BUILD_OPT_SUFFIX ""
#else
#define ISH_BUILD_OPT_SUFFIX " unoptimized"
#endif

// Same idea, same reason, for the arm64-guest gadget dispatch mode selected by
// -Darm64_gret (see jit/guest-arm64/gadgets.h). A build whose dispatch
// instruction you cannot see makes a dispatch A/B unfalsifiable: a flat result
// is then indistinguishable from "the option never took effect", which is
// exactly the ambiguity this suffix exists to remove. Empty for the 'ldar'
// default so normal builds read unchanged, following ISH_BUILD_OPT_SUFFIX.
#if defined(ISH_ARM64_GRET_DMB)
#define ISH_BUILD_GRET_SUFFIX " gret=dmb"
#else
#define ISH_BUILD_GRET_SUFFIX ""
#endif
// The same build stamp copyBuildVersion() formats, as a time_t: the running
// executable's own mtime, which moves on every relink. 0 if the host won't say.
// aokfs uses it as the mtime of everything it synthesizes (see fs/aok.c), so
// `ls -l /AOK` and `uname -v` describe the same build.
time_t buildTimestamp(void);
char *copyHostArchitecture(void);
char *copyHostMachineIdentifier(void);
char *copyHostDeviceName(void);
char *copyHostCoreTopology(void);

// Real cache geometry of the host CPU, for /sys/devices/system/cpu/cpuN/cache.
// Any field the host will not tell us about is left 0, and the corresponding
// sysfs attribute is then omitted rather than invented.
struct host_cache_geometry {
    uint64_t l1i_size; // bytes
    uint64_t l1d_size; // bytes
    uint64_t l2_size;  // bytes
    uint64_t line_size; // bytes
};

void hostCacheGeometry(struct host_cache_geometry *out);

#endif
