#!/bin/bash
# Publish the App Store listing's two pages to the gh-pages branch, where
# GitHub Pages serves them:
#
#   docs/privacy-policy.md  https://emkey1.github.io/AOK-Workstation/privacy/
#                           (Settings -> Privacy Policy, and the listing)
#   docs/accessibility.md   https://emkey1.github.io/AOK-Workstation/accessibility/
#                           (the listing's Accessibility URL)
#
# The branch holds only what Pages serves: those pages and a landing page. It
# is never built from docs/ on `working`, which would publish every planning
# document in it. Pages renders the Markdown itself (Jekyll), so each page is
# copied as written, with front matter on top.
#
#   tools/publish-pages.sh          commit to gh-pages and push
#   tools/publish-pages.sh --dry    build the commit, do not push
#
# Run it after every change to either page; the policy's own "Changes to this
# policy" section promises the published copy follows it, and the
# accessibility page backs what the App Store's Accessibility Nutrition Label
# claims. tools/publish-privacy-policy.sh is the old name and runs this.
set -euo pipefail

top=$(git rev-parse --show-toplevel)
cd "$top"
policy=docs/privacy-policy.md
accessibility=docs/accessibility.md
for page in "$policy" "$accessibility"; do
    [ -f "$page" ] || { echo "$0: no $page" >&2; exit 1; }
done
push=1
[ "${1:-}" = --dry ] && push=0

work=$(mktemp -d "${TMPDIR:-/tmp}/aok-gh-pages.XXXXXX")
trap 'git worktree remove --force "$work" >/dev/null 2>&1 || true; rm -rf "$work"' EXIT

git fetch -q origin gh-pages 2>/dev/null || true
if git rev-parse -q --verify origin/gh-pages >/dev/null; then
    git worktree add -q --detach "$work" origin/gh-pages
else
    git worktree add -q --detach "$work"
    git -C "$work" checkout -q --orphan gh-pages-new
    git -C "$work" rm -rqf . >/dev/null 2>&1 || true
fi

mkdir -p "$work/privacy"
{
    printf -- '---\ntitle: AOK-Workstation privacy policy\n---\n\n'
    cat "$policy"
} > "$work/privacy/index.md"

mkdir -p "$work/accessibility"
{
    printf -- '---\ntitle: AOK-Workstation accessibility\n---\n\n'
    cat "$accessibility"
} > "$work/accessibility/index.md"

# The listing's own words (docs/app_store_metadata.md, Subtitle): never that
# the app "is" or "runs" Linux.
cat > "$work/index.md" <<'EOF'
---
title: AOK-Workstation
---

# AOK-Workstation

A Unix-style shell on device, for iPhone and iPad.

- [Accessibility](accessibility/)
- [Privacy policy](privacy/)
- [Source code](https://github.com/emkey1/AOK-Workstation)
- [Issues](https://github.com/emkey1/AOK-Workstation/issues)
EOF

cat > "$work/_config.yml" <<'EOF'
title: AOK-Workstation
theme: jekyll-theme-primer
EOF

git -C "$work" add -A
if git -C "$work" diff --cached --quiet; then
    echo "gh-pages already has these pages; nothing to publish."
    exit 0
fi
src=$(git rev-parse --short HEAD)
git -C "$work" commit -q -m "Pages from $src

$(git log -1 --format='%h %s' -- "$policy")
$(git log -1 --format='%h %s' -- "$accessibility")"
git -C "$work" show --stat --oneline HEAD | head -8
if [ "$push" = 1 ]; then
    git -C "$work" push -q origin HEAD:refs/heads/gh-pages
    echo "pushed; Pages serves them in a minute or two:"
    echo "  https://emkey1.github.io/AOK-Workstation/privacy/"
    echo "  https://emkey1.github.io/AOK-Workstation/accessibility/"
fi
