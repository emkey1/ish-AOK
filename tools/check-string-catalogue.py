#!/usr/bin/env python3
"""Find UI strings the nine translations have fallen behind on.

Why this exists
---------------
A string catalogue does not fail a build when it is out of date. Add an
NSLocalizedString and the English shows; change one and the old key keeps its
nine translations while the new key has none; delete the code and the key stays
for ever. Every one of those ships, silently, as English text inside a German,
Japanese or Chinese UI -- and the only person who sees it is a user who does
not read English.

That is exactly what happened in the 566 cycle. Replacing the Suspend and Exit
confirmation (02774847b) left its nine translations attached to a key nothing
referenced any more, and the new wording untranslated; deleting the repository
upgrade screen (7f79160b2) left nine more keys behind it. Nothing noticed until
this script was written.

Xcode's own extraction does not close the gap: it writes .stringsdata into the
build directory, not back into the source catalogue, so the repository's
catalogues are maintained by hand and need checking by hand.

A storyboard label is the same trap wearing different clothes, and it caught
the Privacy Policy and Legal rows: their text is in About.storyboard, so the
catalogue behind them is About.xcstrings, and adding an NSLocalizedString of
the same words to Localizable.xcstrings translates nothing. Both rows sat in
English in the middle of a German Settings screen. So each storyboard is
exported with ibtool and compared with its own catalogue too.

    tools/check-string-catalogue.py

Reports, and exits 1 on, strings referenced in app/ but absent from
Localizable.xcstrings, storyboard strings absent from their catalogue, and
strings in any catalogue that some language has no translation for. Catalogue
keys with no caller in app/*.m are reported without failing: Swift and
Info.plist reach the same catalogues, and LIVE_ELSEWHERE below names the ones
that are known to. A string that genuinely should not be translated -- the
app's own name, a key cap like the tab arrow, a placeholder the code
overwrites -- is marked shouldTranslate: false in its catalogue, which Xcode
shows as "Don't Translate" and this script then leaves alone.
"""
import json
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(REPO, 'app')

# Keys whose only caller is outside the sources this script reads. Swift App
# Intents strings are literals in a property, not NSLocalizedString calls, so
# there is nothing here to match them against; they are listed rather than
# guessed at so that a key going stale still shows up.
LIVE_ELSEWHERE = {
    'Command': 'app/ISHRunCommandIntent.swift @Parameter(title:)',
    'Dashboard': 'app/ISHAppShortcuts.swift AppShortcut(shortTitle:)',
    'Destination': 'app/ISHAppShortcuts.swift @Parameter(title:)',
    'Fail on Non-Zero Exit': 'app/ISHRunCommandIntent.swift @Parameter(title:)',
    'Open iSH-AOK': 'app/ISHAppShortcuts.swift AppIntent title',
    'Run Command': 'app/ISHRunCommandIntent.swift AppIntent title',
    'Timeout (seconds)': 'app/ISHRunCommandIntent.swift @Parameter(title:)',
    'Workspace Dashboard': 'app/ISHAppShortcuts.swift DisplayRepresentation',
}

# Catalogues whose keys come from a storyboard or Info.plist, not from code.
COVERAGE_ONLY = ['About.xcstrings', 'InfoPlist.xcstrings', 'Roots.xcstrings',
                 'Terminal.xcstrings']

# Each storyboard's keys belong to the catalogue of the same name.
STORYBOARDS = os.path.join(APP, 'Base.lproj')

SIMPLE = {'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'", '0': '\0', 'r': '\r'}
CALL = re.compile(r'NSLocalizedString\s*\(')


def skip_blanks(s, i):
    while i < len(s):
        if s[i].isspace():
            i += 1
        elif s.startswith('//', i):
            j = s.find('\n', i)
            i = len(s) if j < 0 else j + 1
        elif s.startswith('/*', i):
            j = s.find('*/', i)
            i = len(s) if j < 0 else j + 2
        else:
            break
    return i


def read_quoted(s, i):
    """Read one "..." body, i at the opening quote. Returns (text, next_i)."""
    i += 1
    buf = []
    while i < len(s):
        c = s[i]
        if c == '\\':
            nxt = s[i + 1]
            if nxt == 'u':
                buf.append(chr(int(s[i + 2:i + 6], 16)))
                i += 6
            elif nxt == 'U':
                buf.append(chr(int(s[i + 2:i + 10], 16)))
                i += 10
            else:
                buf.append(SIMPLE.get(nxt, '\\' + nxt))
                i += 2
            continue
        if c == '"':
            return ''.join(buf), i + 1
        buf.append(c)
        i += 1
    return ''.join(buf), i


def read_literal(s, i):
    """Read a run of adjacent literals. The first must be @"..."; the rest may
    be bare "...", which concatenates with an NSString literal in ObjC."""
    parts = []
    while True:
        i = skip_blanks(s, i)
        if i + 1 < len(s) and s[i] == '@' and s[i + 1] == '"':
            text, i = read_quoted(s, i + 1)
        elif parts and i < len(s) and s[i] == '"':
            text, i = read_quoted(s, i)
        else:
            break
        parts.append(text)
    if not parts:
        return None
    return ''.join(parts), i


def keys_in_code():
    found = {}
    for name in sorted(os.listdir(APP)):
        if not name.endswith(('.m', '.h', '.c')):
            continue
        path = os.path.join(APP, name)
        with open(path, encoding='utf-8', errors='replace') as fh:
            src = fh.read()
        for m in CALL.finditer(src):
            got = read_literal(src, m.end())
            if got is not None:
                found.setdefault(got[0], set()).add(name)
    return found


def load(name):
    with open(os.path.join(APP, name), encoding='utf-8') as fh:
        return json.load(fh)


def coverage_gaps(doc):
    """(key, [languages with no usable translation]) for each short key."""
    languages = {lang for entry in doc['strings'].values()
                 for lang in entry.get('localizations', {})}
    languages.discard(doc.get('sourceLanguage', 'en'))
    gaps = []
    for key, entry in sorted(doc['strings'].items()):
        if entry.get('shouldTranslate') is False:
            continue
        locs = entry.get('localizations', {})
        if not locs:
            # A key no language has been given at all: Xcode shows the source
            # string everywhere, which is the same symptom, so report it.
            gaps.append((key, sorted(languages)))
            continue
        missing = [lang for lang in sorted(languages)
                   if locs.get(lang, {}).get('stringUnit', {}).get('state')
                   != 'translated']
        if missing:
            gaps.append((key, missing))
    return gaps, sorted(languages)


def storyboard_keys(path):
    """The keys ibtool would localize for one storyboard: {ObjectID.prop: text}."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'exported.strings')
        subprocess.run(['ibtool', '--export-strings-file', out, path],
                       check=True, capture_output=True)
        if not os.path.exists(out):
            return {}
        text = subprocess.run(['plutil', '-convert', 'json', '-o', '-', out],
                              check=True, capture_output=True, text=True).stdout
    return json.loads(text) if text.strip() else {}


def check_storyboards():
    bad = False
    for name in sorted(os.listdir(STORYBOARDS)):
        if not name.endswith('.storyboard'):
            continue
        catalogue = name[:-len('.storyboard')] + '.xcstrings'
        if not os.path.exists(os.path.join(APP, catalogue)):
            continue
        exported = storyboard_keys(os.path.join(STORYBOARDS, name))
        have = load(catalogue)['strings']
        missing = sorted(set(exported) - set(have))
        if missing:
            bad = True
            print(f'\n{name}: {len(missing)} string(s) are not in '
                  f'{catalogue} -- they will show in English in every '
                  f'language:')
            for key in missing:
                print(f'  {key} = {exported[key]!r}')
        stale = sorted(set(have) - set(exported))
        if stale:
            print(f'\n{catalogue}: {len(stale)} key(s) name an object that is '
                  f'no longer in {name}:')
            for key in stale:
                value = (have[key].get('localizations', {}).get('en', {})
                         .get('stringUnit', {}).get('value'))
                print(f'  {key} = {value!r}')
    return bad


def main():
    bad = False
    doc = load('Localizable.xcstrings')
    catalogue = set(doc['strings'])
    code = keys_in_code()

    missing = sorted(set(code) - catalogue)
    if missing:
        bad = True
        print(f'{len(missing)} string(s) used in app/ are not in '
              f'Localizable.xcstrings -- they will show in English in every '
              f'language:')
        for key in missing:
            print(f'  {key!r}\n      {", ".join(sorted(code[key]))}')

    unused = sorted(catalogue - set(code) - set(LIVE_ELSEWHERE))
    if unused:
        print(f'\n{len(unused)} key(s) in Localizable.xcstrings with no '
              f'NSLocalizedString in app/. Check Swift, the storyboards and '
              f'Info.plist before deleting one; add it to LIVE_ELSEWHERE if '
              f'it is live:')
        for key in unused:
            print(f'  {key!r}')

    stale = sorted(set(LIVE_ELSEWHERE) - catalogue)
    if stale:
        bad = True
        print(f'\n{len(stale)} key(s) in LIVE_ELSEWHERE are no longer in the '
              f'catalogue; drop them from this script:')
        for key in stale:
            print(f'  {key!r} -- {LIVE_ELSEWHERE[key]}')

    bad = check_storyboards() or bad

    for name in ['Localizable.xcstrings'] + COVERAGE_ONLY:
        gaps, languages = coverage_gaps(load(name))
        if gaps:
            bad = True
            print(f'\n{name}: {len(gaps)} string(s) are not translated into '
                  f'every one of {len(languages)} languages:')
            for key, langs in gaps:
                print(f'  {key[:70]!r}\n      missing: {" ".join(langs)}')

    if not bad:
        print(f'Localizable.xcstrings: {len(catalogue)} keys, all of the '
              f'{len(code)} used in app/ present. Every storyboard matches its '
              f'catalogue, and every string is translated into all nine '
              f'languages or marked Do Not Translate.')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
