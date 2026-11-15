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

Swift is a third surface, and a quieter one: App Intents put their text in
properties -- `static var title: LocalizedStringResource = "Run Command"`,
`@Parameter(title:)`, the Siri phrases -- so there is no NSLocalizedString to
grep for, and the keys Xcode derives from an interpolation are not the source
text -- an interpolated `\\(seconds)` becomes `%lld`. Guessing them is how
dead weight gets into a catalogue instead of a translation, so SWIFT_KEYS
below is a snapshot of what the build itself extracts, and this script
re-reads that extraction when it can find it and fails if the two have
drifted apart. The build writes it only because SWIFT_EMIT_LOC_STRINGS is set
in app/App.xcconfig.

The Siri phrases are a fourth surface with a format of its own: a build
refuses AppShortcuts.xcstrings below iOS 17 and this app targets 15, so the
phrases live in app/<lang>.lproj/AppShortcuts.strings -- ten legacy tables,
checked here against each other and against the build's AppShortcuts
extraction.

A storyboard label is the same trap wearing different clothes, and it caught
the Privacy Policy and Legal rows: their text is in About.storyboard, so the
catalogue behind them is About.xcstrings, and adding an NSLocalizedString of
the same words to Localizable.xcstrings translates nothing. Both rows sat in
English in the middle of a German Settings screen. So each storyboard is
exported with ibtool and compared with its own catalogue too.

    tools/check-string-catalogue.py

Reports, and exits 1 on, strings referenced in app/ but absent from
Localizable.xcstrings, storyboard strings absent from their catalogue, Swift
keys that have drifted from what the build extracts, a Siri phrase table that
disagrees with the others, and strings in any catalogue that some language has
no translation for. Catalogue keys with no caller in app/*.m are reported
without failing: Swift and Info.plist reach the same catalogues, and
SWIFT_KEYS below names the ones that are known to. A string that genuinely
should not be translated -- the app's own name, a key cap like the tab arrow,
a placeholder the code overwrites -- is marked shouldTranslate: false in its
catalogue, which Xcode shows as "Don't Translate" and this script then leaves
alone.
"""
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(REPO, 'app')

# Keys whose only caller is outside the sources this script reads: the Swift
# App Intents, whose text is a literal in a property rather than an
# NSLocalizedString call. Not typed out by hand -- this is the Localizable
# table of the .stringsdata the build writes, which is why it holds the keys
# Xcode derives from an interpolation (%lld, %@) rather than the Swift source
# text. swift_keys_from_build() re-reads that extraction and reports any
# difference, so this list cannot quietly go stale; regenerate it from the
# paths in STRINGSDATA when it does.
SWIFT_KEYS = {
    '%@': 'app/ISHRunCommandIntent.swift',
    'Boot Images': 'app/ISHAppShortcuts.swift',
    'Browser': 'app/ISHAppShortcuts.swift',
    'Command': 'app/ISHRunCommandIntent.swift',
    'Dashboard': 'app/ISHAppShortcuts.swift',
    'Fail on Non-Zero Exit': 'app/ISHRunCommandIntent.swift',
    'Open iSH-AOK': 'app/ISHAppShortcuts.swift',
    'Run ${command} in iSH-AOK': 'the App Shortcuts metadata extraction',
    'Run Command': 'app/ISHRunCommandIntent.swift',
    'Running commands from Shortcuts is turned off. Enable \u201cAllow '
    'Shortcuts to Run Commands\u201d in iSH-AOK\u2019s settings.':
        'app/ISHRunCommandIntent.swift',
    'Runs a shell command in the iSH-AOK guest system and returns its output '
    '(stdout and stderr merged). The command runs headlessly; the app does '
    'not need to be open. It runs as root, or as the default user account '
    'when "Open Everything as Default User" is enabled in Settings.':
        'app/ISHRunCommandIntent.swift',
    'Session Shell': 'app/ISHAppShortcuts.swift',
    'Shortcuts Destination': 'app/ISHAppShortcuts.swift',
    'System Console': 'app/ISHAppShortcuts.swift',
    'The command exited with status %lld.': 'app/ISHRunCommandIntent.swift',
    'The command exited with status %lld. Output: %@':
        'app/ISHRunCommandIntent.swift',
    'The command is empty.': 'app/ISHRunCommandIntent.swift',
    'The command timed out after %lld seconds.':
        'app/ISHRunCommandIntent.swift',
    'The command timed out after %lld seconds. Partial output: %@':
        'app/ISHRunCommandIntent.swift',
    'The command was killed by signal %lld.': 'app/ISHRunCommandIntent.swift',
    'The command was killed by signal %lld. Partial output: %@':
        'app/ISHRunCommandIntent.swift',
    "The command's exit status could not be determined, so Fail on Non-Zero "
    "Exit cannot vouch for it.": 'app/ISHRunCommandIntent.swift',
    "The command's exit status could not be determined, so Fail on Non-Zero "
    "Exit cannot vouch for it. Output: %@": 'app/ISHRunCommandIntent.swift',
    'Themes': 'app/ISHAppShortcuts.swift',
    'Timeout (seconds)': 'app/ISHRunCommandIntent.swift',
    'Workspace Dashboard': 'app/ISHAppShortcuts.swift',
}

# What the compiler and the App Shortcuts metadata processor write, under
# whichever DerivedData directory this checkout last built into. Present only
# after a build of the app; absent on a machine that has only cloned.
STRINGSDATA = ('~/Library/Developer/Xcode/DerivedData/iSH-AOK-*/Build/'
               'Intermediates.noindex/iSH-AOK.build/*/iSH-AOK.build/'
               'Objects-normal/*/%s.stringsdata')
EXTRACTORS = ('ISHAppShortcuts', 'ISHRunCommandIntent',
              'ExtractedAppShortcutsMetadata')

# The Siri phrases, one legacy .strings table per language.
PHRASE_TABLE = 'AppShortcuts.strings'
PHRASE_LANGS = ['en', 'de', 'es', 'fr', 'ja', 'ko', 'pt-BR', 'ru',
                'zh-Hans', 'zh-Hant']
APP_NAME_TOKEN = '${applicationName}'

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


def plist(path):
    """One .strings (or any plist) as a dict."""
    text = subprocess.run(['plutil', '-convert', 'json', '-o', '-', path],
                          check=True, capture_output=True, text=True).stdout
    return json.loads(text) if text.strip() else {}


def swift_keys_from_build():
    """{table: {key}} from the .stringsdata of the newest build, or None.

    The compiler writes one file per Swift source and the App Shortcuts
    metadata processor writes another; between them they are the whole Swift
    surface, keys and tables exactly as the product will look them up."""
    tables = {}
    found = False
    for name in EXTRACTORS:
        paths = glob.glob(os.path.expanduser(STRINGSDATA % name))
        if not paths:
            continue
        found = True
        doc = plist(max(paths, key=os.path.getmtime))
        for table, entries in doc.get('tables', {}).items():
            for entry in entries:
                tables.setdefault(table, set()).add(entry['key'])
    return tables if found else None


def check_swift(catalogue, build):
    """SWIFT_KEYS against the catalogue, and against the build if there is
    one."""
    bad = False
    stale = sorted(set(SWIFT_KEYS) - catalogue)
    if stale:
        bad = True
        print(f'\n{len(stale)} key(s) in SWIFT_KEYS are no longer in the '
              f'catalogue; they will show in English in every language:')
        for key in stale:
            print(f'  {key[:70]!r} -- {SWIFT_KEYS[key]}')
    if build is None:
        print('\nThe Swift keys were not checked against the build: no '
              '.stringsdata under DerivedData. Build the iSH-AOK scheme and '
              'run this again to compare them.')
        return bad
    extracted = build.get('Localizable', set())
    for label, keys in (('are extracted by the build but not in SWIFT_KEYS',
                         sorted(extracted - set(SWIFT_KEYS))),
                        ('are in SWIFT_KEYS but no longer extracted',
                         sorted(set(SWIFT_KEYS) - extracted))):
        if keys:
            bad = True
            print(f'\n{len(keys)} Swift key(s) {label}; regenerate the list '
                  f'from the .stringsdata:')
            for key in keys:
                print(f'  {key[:70]!r}')
    return bad


def check_phrases(build):
    """The ten AppShortcuts.strings tables, against each other and the
    build."""
    bad = False
    tables = {}
    for lang in PHRASE_LANGS:
        path = os.path.join(APP, lang + '.lproj', PHRASE_TABLE)
        if not os.path.exists(path):
            bad = True
            print(f'\n{lang}.lproj/{PHRASE_TABLE} is missing: Siri will not '
                  f'recognise an iSH-AOK phrase spoken in {lang}.')
            continue
        tables[lang] = plist(path)

    reference = tables.get('en')
    if reference is None:
        return True
    for lang, table in sorted(tables.items()):
        missing = sorted(set(reference) - set(table))
        extra = sorted(set(table) - set(reference))
        if missing or extra:
            bad = True
            print(f'\n{lang}.lproj/{PHRASE_TABLE} does not carry the same '
                  f'phrases as en.lproj/{PHRASE_TABLE}:')
            for key in missing:
                print(f'  not translated: {key!r}')
            for key in extra:
                print(f'  not a phrase in the source: {key!r}')
        for key, value in sorted(table.items()):
            if APP_NAME_TOKEN not in value:
                bad = True
                print(f'\n{lang}.lproj/{PHRASE_TABLE}: {value!r} has no '
                      f'{APP_NAME_TOKEN}, so Siri cannot match it.')

    if build is not None:
        # The extraction names the FIRST phrase of each AppShortcut as the
        # key; the rest arrive in its values, so this is a subset check, not
        # an equality one.
        extracted = build.get('AppShortcuts', set())
        unknown = sorted(extracted - set(reference))
        if unknown:
            bad = True
            print(f'\n{len(unknown)} phrase(s) in the source are in no '
                  f'{PHRASE_TABLE} table:')
            for key in unknown:
                print(f'  {key!r}')
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

    unused = sorted(catalogue - set(code) - set(SWIFT_KEYS))
    if unused:
        print(f'\n{len(unused)} key(s) in Localizable.xcstrings with no '
              f'NSLocalizedString in app/. Check Swift, the storyboards and '
              f'Info.plist before deleting one; add it to SWIFT_KEYS if '
              f'it is live:')
        for key in unused:
            print(f'  {key!r}')

    build = swift_keys_from_build()
    bad = check_swift(catalogue, build) or bad
    bad = check_phrases(build) or bad
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
              f'{len(code)} used in app/ and all {len(SWIFT_KEYS)} the Swift '
              f'App Intents reach present. Every storyboard matches its '
              f'catalogue, the Siri phrases are in all '
              f'{len(PHRASE_LANGS)} {PHRASE_TABLE} tables, and every string '
              f'is translated into all nine languages or marked Do Not '
              f'Translate.')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
