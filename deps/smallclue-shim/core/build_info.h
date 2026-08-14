#ifndef BUILD_INFO_H
#define BUILD_INFO_H
/* smallclue's own CMakeLists writes this file when it is absent. AOK does not
 * use that build system, so it is provided here instead -- same declarations,
 * kept out of deps/smallclue so the submodule stays pristine. */
#define BUILD_VERSION "iSH-AOK-native"
#include <stdbool.h>
#include <stddef.h>
bool pscalRuntimeStderrIsInteractive(void);
const char *pscal_program_version_string(void);
bool pathTruncateEnabled(void);
bool pathTruncateStrip(const char *path, char *buffer, size_t buflen);
#endif
