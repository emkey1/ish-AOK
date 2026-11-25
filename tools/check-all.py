#!/usr/bin/env python3
"""Run every tools/check-* in one go, which is what a release tag needs.

Why this exists
---------------
The checkers in tools/ were each written the day something shipped broken, and
each of them would have caught the thing it is named after. None of them was
run by anything. tools/check-string-catalogue.py said so in its own header for
a fortnight while the Shortcuts strings went out untranslated, and
tools/check-audio-graph.py was written three days after the music player
stopped playing anything at all. A check nobody runs is a comment.

So: one command before a tag, and one step in the release workflow that will
not build an IPA until it passes.

    tools/check-all.py              # everything that applies to this machine
    tools/check-all.py --only audio-graph string-catalogue
    tools/check-all.py --list

Each check is run as its own process, its output is kept and printed only if it
fails, and the exit code is 1 if any of them failed. Checks that need Xcode,
ibtool or AVFoundation are skipped with a note on anything that is not macOS
rather than counted as failures, so this is safe to call from a Linux job too.
"""

import argparse
import glob
import os
import platform
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SELF = os.path.basename(__file__)

# Checks that cannot run away from a Mac with Xcode: ibtool and plutil for the
# storyboards and the catalogues, the iOS device database for the device table,
# AVFoundation for the audio graph.
DARWIN_ONLY = {
    'check-string-catalogue.py',
    'check-device-table.py',
    'check-audio-graph.py',
}

# Checks that read build output rather than source. These have to run AFTER a
# build or they examine nothing -- and check-bash-tls.py says "no archives to
# check (build first)" and exits 0 when it finds none, so running it too early
# does not fail, it just quietly stops being a check. Its own header calls that
# out as the failure the whole file exists to prevent, which is exactly why the
# runner knows the difference instead of leaving it to whoever writes the job.
NEEDS_BUILD = {
    'check-bash-tls.py': 'reads the built archives',
    # Was missing here until 566, whose release build failed at the source
    # stage with "build/libsmallclue.a is not built": nothing is, that early.
    'check-native-libc.py': 'reads the built archives',
    # cargo tree runs offline, so the crates must already be fetched.
    'check-licences.py': 'needs the crates a build fetches',
}

# What each one is for, in one line, so a red run says what broke without
# anybody having to open the script.
ABOUT = {
    'check-audio-graph.py':
        'the music player cannot come up with its graph connected to nothing',
    'check-bash-tls.py':
        'no bash global is __thread in one translation unit and not another',
    'check-licences.py':
        'the Licences screen lists everything the build puts in the app',
    'check-device-table.py':
        'kernel/hostinfo.m knows every device Xcode can build for',
    'check-native-libc.py':
        'the native libc shim agrees with the kernel about its structs',
    'check-shared-gadgets.sh':
        'the gadget sources shared with the VM have not diverged',
    'check-shell-quoting.sh':
        'the shell scripts quote what they expand',
    'check-string-catalogue.py':
        'every user-visible string is in a catalogue and translated',
}


def discover():
    names = []
    for path in sorted(glob.glob(os.path.join(HERE, 'check-*.py')) +
                       glob.glob(os.path.join(HERE, 'check-*.sh'))):
        name = os.path.basename(path)
        if name != SELF:
            names.append(name)
    return names


def run(name):
    """One check, as its own process. Returns (status, seconds, output)."""
    path = os.path.join(HERE, name)
    # Invoked through the interpreter rather than relying on the execute bit:
    # check-bash-tls.py is in the tree without one, and a check that silently
    # does not run is the thing this script exists to stop.
    argv = [sys.executable, path] if name.endswith('.py') else ['/bin/sh', path]
    started = time.monotonic()
    done = subprocess.run(argv, capture_output=True, text=True, cwd=os.path.dirname(HERE))
    elapsed = time.monotonic() - started
    output = (done.stdout or '') + (done.stderr or '')
    return ('pass' if done.returncode == 0 else 'fail'), elapsed, output


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--only', nargs='+', metavar='NAME',
                        help='run just these, named with or without check-/.py')
    parser.add_argument('--list', action='store_true', help='name them and stop')
    stage = parser.add_mutually_exclusive_group()
    stage.add_argument('--source-only', action='store_true',
                       help='skip the checks that read build output')
    stage.add_argument('--build-only', action='store_true',
                       help='only the checks that read build output')
    args = parser.parse_args()

    names = discover()
    if args.source_only:
        names = [n for n in names if n not in NEEDS_BUILD]
    elif args.build_only:
        names = [n for n in names if n in NEEDS_BUILD]
    if args.only:
        wanted = set()
        for raw in args.only:
            stem = raw[len('check-'):] if raw.startswith('check-') else raw
            stem = os.path.splitext(stem)[0]
            matches = [n for n in names
                       if os.path.splitext(n)[0][len('check-'):] == stem]
            if not matches:
                print(f'no check named {raw!r}; --list shows them')
                return 2
            wanted.update(matches)
        names = [n for n in names if n in wanted]

    if args.list:
        for name in names:
            tags = []
            if name in DARWIN_ONLY:
                tags.append('macOS only')
            if name in NEEDS_BUILD:
                tags.append(NEEDS_BUILD[name])
            suffix = f' ({"; ".join(tags)})' if tags else ''
            print(f'{name}{suffix}\n    {ABOUT.get(name, "")}')
        return 0

    darwin = platform.system() == 'Darwin'
    failed, skipped, results = [], [], []
    for name in names:
        if name in DARWIN_ONLY and not darwin:
            skipped.append(name)
            print(f'  SKIP  {name}  (needs macOS)')
            continue
        # Only on a terminal: in a log the carriage return is just a character,
        # and the line ends up printed twice.
        if sys.stdout.isatty():
            print(f'  ....  {name}', end='\r', flush=True)
        status, elapsed, output = run(name)
        print(f'  {status.upper():4}  {name}  ({elapsed:.0f}s)')
        results.append((name, status, output))
        if status == 'fail':
            failed.append(name)

    for name, status, output in results:
        if status == 'fail':
            print(f'\n===== {name}: {ABOUT.get(name, "failed")} =====')
            print(output.rstrip() or '(no output)')

    print()
    if skipped:
        print(f'{len(skipped)} skipped, not being on macOS: '
              f'{", ".join(skipped)}')
    if failed:
        print(f'{len(failed)} of {len(results)} checks failed: '
              f'{", ".join(failed)}. Do not tag this.')
        return 1
    print(f'All {len(results)} checks pass.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
