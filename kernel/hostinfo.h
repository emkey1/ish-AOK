#ifndef ISH_HOSTINFO_H
#define ISH_HOSTINFO_H

#include <stdint.h>

char *printHostInfo(void);
// Identifies the build itself, for uname -v. On iOS that is the app's version
// and build number ("1.3 (546)"), which is what tells you which build a device
// is actually running; elsewhere it is the compile timestamp. Caller frees.
char *copyBuildVersion(void);
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
