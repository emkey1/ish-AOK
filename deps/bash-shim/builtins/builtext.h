/* Forwarding header.
 *
 * bash includes <builtins/builtext.h>, and mkbuiltins generates that file into
 * the build tree -- where meson will not put it, because a custom_target output
 * may not contain a path segment. So this sits on the include path AHEAD of
 * deps/bash and points at the generated file by its flat name.
 *
 * Here rather than in deps/bash so the submodule stays a byte-for-byte vendor
 * drop and `git diff` against upstream keeps meaning what it says. Same
 * arrangement, and same reason, as deps/smallclue-shim.
 */
#include "bash_builtext.h"
