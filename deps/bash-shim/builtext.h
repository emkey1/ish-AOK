/* Same forwarding header as builtins/builtext.h, under the name the generated
 * builtins use.
 *
 * Top-level bash sources say <builtins/builtext.h>; the per-builtin files that
 * mkbuiltins produces say "builtext.h", because in bash's own tree they sit in
 * builtins/ beside it. Both spellings have to land on the generated file.
 */
#include "bash_builtext.h"
