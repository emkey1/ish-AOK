#!/usr/bin/env python3
"""Publish opt/AOK/docs to the AOK-iSH GitHub wiki, without losing edits made there.

    tools/sync-wiki.py               # report only: say what would change, touch nothing
    tools/sync-wiki.py --push        # publish, and push
    tools/sync-wiki.py --adopt       # accept the wiki's current text as the baseline

WHY THIS IS NOT A COPY LOOP.

A GitHub wiki is a git repository people can edit through the web UI, and this
one already has been: `Home.md` carries a hand-written "Developer Instructions"
document that exists nowhere in opt/AOK/docs, and `Crypto-Acceleration.md` had
drifted 318 diff lines from crypto-accel.md before this script existed. A plain
publish would have destroyed both.

So every page carries a recorded fingerprint of what this script last published
(docs/.wiki-sync). On each run a page is in exactly one of four states:

  new        no such wiki page          -> create it
  unchanged  wiki text == what we published last time, and the doc has not
             changed either                                  -> nothing to do
  ours       wiki text == what we published last time, but the doc has changed
                                                              -> safe to update
  DIVERGED   wiki text != what we published last time         -> somebody edited
             the page. Reported with a diff and SKIPPED.

DIVERGED is the whole point. Those edits are somebody's work, and the fix is to
carry them back into opt/AOK/docs -- the docs are compiled into the app and
served at /AOK/docs, so the repo has to be the source of truth or the shipped
copy silently rots. Once that is done the page publishes cleanly on the next
run. `--adopt` is the escape hatch for a page whose wiki text is already the one
you want to keep: it records the fingerprint without changing anything, so the
page stops being reported and the NEXT doc change will overwrite it.

Home.md is never rewritten. Only the block between the two index markers is
replaced, and if they are absent the page is left alone and reported.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(REPO, "opt", "AOK", "docs")
MANIFEST = os.path.join(REPO, "docs", "wiki-pages.manifest")
STATE = os.path.join(REPO, "docs", ".wiki-sync")
WIKI_URL = "https://github.com/emkey1/ish-AOK.wiki.git"
BOOK_URL = "https://github.com/emkey1/ish-AOK/blob/working/docs/book"

INDEX_BEGIN = "<!-- AOK-DOCS-INDEX:BEGIN -->"
INDEX_END = "<!-- AOK-DOCS-INDEX:END -->"

FOOTER = (
    "\n\n---\n\n*This page is generated from `opt/AOK/docs/{source}` in the "
    "[AOK-iSH repository](https://github.com/emkey1/ish-AOK/blob/working/opt/AOK/docs/{source}), "
    "which is what the app compiles in and serves at `/AOK/docs`. Edits made "
    "here are noticed rather than overwritten, but they have to be carried back "
    "into the repository to reach the app.*\n"
)


def run(cmd, cwd=None, check=True):
    return subprocess.run(cmd, cwd=cwd, check=check, capture_output=True, text=True)


def load_manifest():
    pages = []
    with open(MANIFEST) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 2)
            if len(parts) != 3:
                sys.exit("wiki-pages.manifest: expected '<source> <Page> <Title>', "
                         "got: %s" % line)
            pages.append((parts[0], parts[1], parts[2].strip()))
    return pages


def describe(source):
    """The half of the doc's own H1 after the colon, or "".

    Kept out of the manifest on purpose: taken from the doc, it cannot drift
    from the doc. Most H1s are "Title: what it is"; the ones that are not
    simply get no description."""
    try:
        with open(os.path.join(DOCS, source)) as f:
            for line in f:
                if line.startswith("# "):
                    head = line[2:].strip()
                    return head.split(":", 1)[1].strip() if ":" in head else ""
    except OSError:
        pass
    return ""


def render(source, text, by_source):
    """Rewrite the doc's own links into wiki links, and append provenance."""
    # ](roots.md) and ](roots.md#anchor) -> ](Roots) / ](Roots#anchor).
    def link(m):
        target, anchor = m.group(1), m.group(2) or ""
        page = by_source.get(target)
        if page is not None:
            return "](%s%s)" % (page, anchor)
        # book/ and anything else unlisted stays a real URL rather than a
        # wiki page that does not exist. The book is deliberately not published.
        if target.startswith("book/"):
            return "](%s/%s%s)" % (BOOK_URL, target[len("book/"):], anchor)
        return m.group(0)

    text = re.sub(r"\]\(([0-9A-Za-z._/-]+\.md)(#[0-9A-Za-z-]+)?\)", link, text)
    return text.rstrip("\n") + FOOTER.format(source=source)


def sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--push", action="store_true",
                    help="commit and push (default is to report and change nothing)")
    ap.add_argument("--adopt", action="store_true",
                    help="record the wiki's current text as the baseline, changing nothing")
    ap.add_argument("--init-home", action="store_true",
                    help="insert the index markers into Home.md (one time; needs --push)")
    args = ap.parse_args()

    pages = load_manifest()
    by_source = {src: page for src, page, _ in pages}
    state = {}
    if os.path.exists(STATE):
        with open(STATE) as f:
            state = json.load(f)

    tmp = tempfile.mkdtemp(prefix="aok-wiki.")
    wiki = os.path.join(tmp, "wiki")
    try:
        run(["git", "clone", "--quiet", WIKI_URL, wiki])

        created, updated, diverged, untouched = [], [], [], []
        for source, page, _title in pages:
            src_path = os.path.join(DOCS, source)
            if not os.path.exists(src_path):
                sys.exit("%s is in the manifest but not in opt/AOK/docs" % source)
            with open(src_path) as f:
                want = render(source, f.read(), by_source)

            dst_path = os.path.join(wiki, page + ".md")
            recorded = state.get(page)
            if not os.path.exists(dst_path):
                created.append((page, dst_path, want))
                continue
            with open(dst_path) as f:
                have = f.read()
            if recorded is None or sha(have) != recorded:
                diverged.append((page, source, have, want))
                continue
            if have == want:
                untouched.append(page)
            else:
                updated.append((page, dst_path, want))

        # The index block inside Home.md, and nothing else on that page.
        home_path = os.path.join(wiki, "Home.md")
        home_note = None
        if os.path.exists(home_path):
            with open(home_path) as f:
                home = f.read()
            index = "\n".join(
                "- [%s](%s)%s" % (title, page,
                                  (" - " + describe(source)) if describe(source) else "")
                for source, page, title in pages)
            block = "%s\n%s\n%s" % (INDEX_BEGIN, index, INDEX_END)
            if INDEX_BEGIN in home and INDEX_END in home:
                new_home = re.sub(
                    re.escape(INDEX_BEGIN) + r".*?" + re.escape(INDEX_END),
                    block.replace("\\", "\\\\"), home, flags=re.S)
                if new_home != home and args.push:
                    with open(home_path, "w") as f:
                        f.write(new_home)
                home_note = "index block updated" if new_home != home else "index block current"
            elif args.init_home:
                # One-time: wrap the existing "## Pages" list, or append a
                # section if there is none. Everything else on the page --
                # including the hand-written Developer Instructions -- is left
                # exactly as it is.
                m = re.search(r"^## Pages\s*\n(?:^[-*] .*\n|^\s*\n(?=[-*] ))*", home, re.M)
                if m:
                    new_home = home[:m.start()] + "## Pages\n\n" + block + "\n" + home[m.end():]
                else:
                    new_home = home.rstrip("\n") + "\n\n## Pages\n\n" + block + "\n"
                if args.push:
                    with open(home_path, "w") as f:
                        f.write(new_home)
                home_note = "index markers inserted around the page list"
            else:
                home_note = ("NO index markers -- Home.md left completely alone. Add\n"
                             "      %s\n      %s\n"
                             "   where the page list should go, and it will be maintained,\n"
                             "   or run --init-home --push to insert them once."
                             % (INDEX_BEGIN, INDEX_END))

        print("wiki: %s" % WIKI_URL)
        print("  %d new, %d to update, %d diverged, %d already current"
              % (len(created), len(updated), len(diverged), len(untouched)))
        if home_note:
            print("  Home.md: %s" % home_note)
        for page, _, _ in created:
            print("   NEW       %s" % page)
        for page, _, _ in updated:
            print("   UPDATE    %s" % page)
        for page, source, have, want in diverged:
            print("   DIVERGED  %s  (edited on the wiki since the last sync)" % page)
            print("             carry the change back into opt/AOK/docs/%s, or" % source)
            print("             run --adopt to keep the wiki's text as the baseline")

        if diverged and not args.adopt:
            print("\n%d page(s) need vetting before they can be published." % len(diverged))
            print("To see one:  git clone %s && diff <(cat wiki/<Page>.md) opt/AOK/docs/<source>"
                  % WIKI_URL)

        if args.adopt:
            for page, _, have, _ in diverged:
                state[page] = sha(have)
            with open(STATE, "w") as f:
                json.dump(state, f, indent=2, sort_keys=True)
                f.write("\n")
            print("\nadopted %d page(s); their current wiki text is now the baseline"
                  % len(diverged))
            return 0

        if not args.push:
            print("\nreport only. Re-run with --push to publish.")
            return 0

        for page, dst_path, want in created + updated:
            with open(dst_path, "w") as f:
                f.write(want)
            state[page] = sha(want)
        if not run(["git", "status", "--porcelain"], cwd=wiki).stdout.strip():
            print("\nnothing to push.")
            return 0
        run(["git", "add", "-A"], cwd=wiki)
        run(["git", "commit", "-q", "-m",
             "docs: sync %d page(s) from opt/AOK/docs" % (len(created) + len(updated))],
            cwd=wiki)
        run(["git", "push", "--quiet"], cwd=wiki)
        with open(STATE, "w") as f:
            json.dump(state, f, indent=2, sort_keys=True)
            f.write("\n")
        print("\npushed. Commit docs/.wiki-sync so the next run can tell your "
              "edits from someone else's.")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
