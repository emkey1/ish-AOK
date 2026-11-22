#!/bin/bash
# Publish docs/privacy-policy.md to the gh-pages branch, where GitHub Pages
# serves it at https://emkey1.github.io/ish-AOK/privacy/ -- the address the
# app's Settings -> Privacy Policy row and the App Store listing give.
#
# The branch holds only what Pages serves: the policy and a landing page. It is
# never built from docs/ on `working`, which would publish every planning
# document in it. Pages renders the Markdown itself (Jekyll), so the policy is
# copied as written, with front matter on top.
#
#   tools/publish-privacy-policy.sh          commit to gh-pages and push
#   tools/publish-privacy-policy.sh --dry    build the commit, do not push
#
# Run it after every change to docs/privacy-policy.md; the policy's own
# "Changes to this policy" section promises the published copy follows it.
set -euo pipefail

top=$(git rev-parse --show-toplevel)
cd "$top"
policy=docs/privacy-policy.md
[ -f "$policy" ] || { echo "$0: no $policy" >&2; exit 1; }
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
    printf -- '---\ntitle: AOK-iSH privacy policy\n---\n\n'
    cat "$policy"
} > "$work/privacy/index.md"

cat > "$work/index.md" <<'EOF'
---
title: AOK-iSH
---

# AOK-iSH

A Linux environment for iPhone and iPad.

- [Privacy policy](privacy/)
- [Source code](https://github.com/emkey1/ish-AOK)
- [Issues](https://github.com/emkey1/ish-AOK/issues)
EOF

cat > "$work/_config.yml" <<'EOF'
title: AOK-iSH
theme: jekyll-theme-primer
EOF

git -C "$work" add -A
if git -C "$work" diff --cached --quiet; then
    echo "gh-pages already has this policy; nothing to publish."
    exit 0
fi
src=$(git rev-parse --short HEAD)
git -C "$work" commit -q -m "Privacy policy from $src

$(git log -1 --format='%h %s' -- "$policy")"
git -C "$work" show --stat --oneline HEAD | head -8
if [ "$push" = 1 ]; then
    git -C "$work" push -q origin HEAD:refs/heads/gh-pages
    echo "pushed; Pages serves it at https://emkey1.github.io/ish-AOK/privacy/ in a minute or two"
fi
