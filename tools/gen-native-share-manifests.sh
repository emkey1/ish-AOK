#!/bin/sh
# Regenerate fs/aok-terminfo.manifest and fs/aok-zsh-functions.manifest, the
# lists of support files /AOK/native/libs serves beside helix's runtime (see
# the aok_generated_libs target in meson.build). Run from the repository root
# after changing opt/AOK/share/terminfo or bumping deps/zsh.
set -e
cd "$(dirname "$0")/.."

{
    echo "# The terminfo database a native-mode root uses, served read-only at"
    echo "# /AOK/native/libs/terminfo (and linked from /usr/share/terminfo by"
    echo "# kernel/native_root.c). Sources: opt/AOK/share/terminfo, copied from Alpine"
    echo "# 3.24's ncurses-terminfo-base (ncurses' MIT-style licence)."
    echo "# Regenerate this list with tools/gen-native-share-manifests.sh."
    (cd opt/AOK/share/terminfo && find . -type f | sed 's|^\./||' | LC_ALL=C sort)
} > fs/aok-terminfo.manifest

# The parts of zsh's own function tree a shell with no distro under it wants:
# the completion system and the everyday function libraries. Completion/Linux
# and the rest are left out -- they complete tools a native root does not have.
ZSH_DIRS="Completion/Base Completion/Zsh Completion/Unix Functions/Misc
Functions/Prompts Functions/Zle Functions/Chpwd Functions/Math
Functions/Exceptions"
{
    echo "# zsh's function tree for native zsh, served read-only at"
    echo "# /AOK/native/libs/zsh/<dir>/... from deps/zsh (zsh's licence). kernel/"
    echo "# zsh_glue.c adds it to FPATH, after any functions the root has."
    echo "# Regenerate this list with tools/gen-native-share-manifests.sh."
    # compinit and its siblings sit at the top of Completion/, not in a subdir.
    (cd deps/zsh && {
        for f in compinit compaudit compdump compinstall bashcompinit; do
            echo "Completion/$f"
        done
        for d in $ZSH_DIRS; do
            find "$d" -type f ! -name '.distfiles' ! -name '*.orig' ! -name '*.rej'
        done
     } | LC_ALL=C sort)
} > fs/aok-zsh-functions.manifest
