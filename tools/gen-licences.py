#!/usr/bin/env python3
"""Collect the licence of everything built into AOK-Workstation into app/Licences.json.

The app shows that file on its Licences screen (Settings > Licences,
app/LicencesViewController.m). Most licences ask for their notice to travel
with the binary ("Redistributions in binary form must reproduce..."), and a
link to a website does not travel with anything, so the texts themselves ship
in the app, read from the same trees the build compiles:

  - COMPONENTS below: AOK-Workstation itself, and the C, Objective-C and JavaScript
    code linked into the app or carried in it as guest programs. A new
    library, submodule or bundled guest binary needs an entry here.
  - Rust crates: whatever `cargo tree` says the shipped build compiles
    (deps/rust-native, for aarch64-apple-ios, with the features app/iSH.xcconfig
    turns on), each crate's licence files read from the crate itself.
  - The Rust standard library, from the toolchain's own copyright document.

Texts that are in neither the tree nor a crate are vendored in tools/licences/
(see the README there).

    tools/gen-licences.py            rewrite app/Licences.json
    tools/gen-licences.py --check    exit 1 if app/Licences.json is stale

Run it after adding or updating a dependency, a submodule or the Rust
toolchain; the release checklist runs --check. It needs the submodules checked
out, cargo, and the crates already fetched (a normal build has done that): it
runs cargo offline.
"""

import hashlib
import html
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'app', 'Licences.json')
VENDORED = 'tools/licences'


def die(msg):
    sys.exit('gen-licences: ' + msg)


def read(path):
    full = path if os.path.isabs(path) else os.path.join(ROOT, path)
    if not os.path.isfile(full):
        die(f'{path} is missing. If it is in a submodule, check the submodule out.')
    with open(full, encoding='utf-8', errors='replace') as f:
        return f.read()


def first_comment(path):
    """A source file's licence header: its first /* ... */ block that says Copyright."""
    for m in re.finditer(r'/\*(.*?)\*/', read(path), re.S):
        if 'Copyright' in m.group(1):
            lines = [re.sub(r'^\s*\* ?', '', l) for l in m.group(1).split('\n')]
            return '\n'.join(lines).strip('\n')
    die(f'{path} has no comment block with a copyright notice in it')


def grab(path, pattern, fmt=None):
    """A version string from a file, or '' when the file says nothing usable."""
    full = os.path.join(ROOT, path)
    if not os.path.isfile(full):
        return ''
    m = re.search(pattern, read(path), re.M)
    if not m:
        return ''
    return fmt(m) if fmt else m.group(1)


def libarchive_version():
    n = grab('deps/libarchive/build/version', r'^(\d+)')
    return f'{int(n) // 1000000}.{int(n) // 1000 % 1000}.{int(n) % 1000}' if n else ''


def three(path, prefix):
    def get(part):
        return grab(path, rf'#define {prefix}{part}\s+(\d+)')
    parts = [get('MAJOR'), get('MINOR'), get('PATCH')]
    return '.'.join(parts) if all(parts) else ''


def bundled_source(field):
    words = read('opt/AOK/bundled/devuan6-aarch64/wayvnc.source').split()
    return words[words.index(field) + 1] if field in words else ''


def audio_version(name):
    v = grab('deps/audiocodecs-static/build-audiocodecs-apple.sh', rf'^{name}_VER=(\S+)')
    return v


def vendored_xiph(lib, var):
    """Xiph's COPYING files are vendored per version; the build script names the version."""
    v = audio_version(var)
    path = f'{VENDORED}/{lib}-{v}.COPYING'
    if not os.path.isfile(os.path.join(ROOT, path)):
        die(f'deps/audiocodecs-static builds {lib} {v} but {path} is not there: '
            f'vendor that version\'s COPYING (github.com/xiph, tag v{v})')
    return v, path


AOK_PREFACE = (
    'AOK-Workstation is a fork of iSH (https://github.com/ish-app/ish) and includes '
    'code from ish-arm64 (https://github.com/OpenMinis/ish-arm64). All three '
    'are under the GNU General Public License, version 3; contributions to '
    'AOK-Workstation after commit 0e3a4144 are also under version 2, as LICENSE.md '
    'below says. The source of this build can be had as the Legal screen '
    'describes.')


def components():
    ogg_v, ogg = vendored_xiph('libogg', 'OGG')
    vorbis_v, vorbis = vendored_xiph('libvorbis', 'VORBIS')
    opus_v, opus = vendored_xiph('opus', 'OPUS')
    opusfile_v, opusfile = vendored_xiph('opusfile', 'OPUSFILE')
    lzma_v = grab('deps/liblzma-static/README.md', r'liblzma (\d+\.\d+\.\d+)')
    venus_dirs = [d for d in os.listdir(os.path.join(ROOT, 'deps/virglrenderer/subprojects'))
                  if d.startswith('venus-protocol-')]
    venus = 'deps/virglrenderer/subprojects/' + (venus_dirs[0] if venus_dirs else 'venus-protocol')
    venus_header = os.path.join(venus, 'generated', 'vn_protocol_renderer_util.h')
    if not os.path.isfile(os.path.join(ROOT, venus_header)):
        gen = os.path.join(venus, 'generated')
        heads = sorted(f for f in os.listdir(os.path.join(ROOT, gen)) if f.endswith('.h')) \
            if os.path.isdir(os.path.join(ROOT, gen)) else []
        if not heads:
            die(f'{venus} has no generated headers to take its notice from')
        venus_header = os.path.join(gen, heads[0])

    # (group, name, version, licence, url, [texts]); a text is a path, or a
    # (title, string) pair for one that is not a whole file.
    return [
        ('app', 'AOK-Workstation', '', 'GPL-3.0 AND GPL-2.0', 'https://github.com/emkey1/AOK-Workstation',
         [('AOK-Workstation', AOK_PREFACE), 'LICENSE.md', 'LICENSE.IOS',
          (f'{VENDORED}/GPL-3.0.txt'), (f'{VENDORED}/GPL-2.0.txt')]),

        ('components', 'SmallCLUE', '', 'Apache-2.0', 'https://github.com/emkey1/smallclue',
         ['deps/smallclue/NOTICE', 'deps/smallclue/LICENSE',
          ('compat/sys/queue.h', first_comment('deps/smallclue/compat/sys/queue.h'))]),
        ('components', 'OpenSSH',
         grab('deps/smallclue/third-party/openssh/version.h', r'SSH_VERSION\s+"OpenSSH_([^"]+)"'),
         'BSD-2-Clause AND BSD-3-Clause AND ISC AND others', 'https://www.openssh.com',
         ['deps/smallclue/third-party/openssh/LICENCE']),
        ('components', 'libgit2',
         grab('deps/smallclue/third-party/libgit2/include/git2/version.h', r'LIBGIT2_VERSION\s+"([^"]+)"'),
         'GPL-2.0 WITH GCC-exception-2.0', 'https://libgit2.org',
         ['deps/smallclue/third-party/libgit2/COPYING']),
        ('components', 'Nextvi', '', 'ISC', 'https://github.com/kyx0r/nextvi',
         ['deps/smallclue/third-party/nextvi/LICENSE']),
        ('components', 'zsh', grab('deps/zsh/Config/version.mk', r'^VERSION=(\S+)'),
         'MIT-Modern-Variant', 'https://www.zsh.org', ['deps/zsh/LICENCE']),
        ('components', 'dash', grab('deps/dash/configure.ac', r'AC_INIT\(\[dash\],\s*\[([^\]]+)\]'),
         'BSD-3-Clause', 'http://gondor.apana.org.au/~herbert/dash/', ['deps/dash/COPYING']),
        ('components', 'libarchive', libarchive_version(), 'BSD-2-Clause',
         'https://www.libarchive.org', ['deps/libarchive/COPYING']),
        ('components', 'liblzma (XZ Utils)', lzma_v, '0BSD', 'https://tukaani.org/xz/',
         [f'{VENDORED}/xz.COPYING', f'{VENDORED}/xz.COPYING.0BSD']),
        ('components', 'libogg', ogg_v, 'BSD-3-Clause', 'https://xiph.org/ogg/', [ogg]),
        ('components', 'libvorbis', vorbis_v, 'BSD-3-Clause', 'https://xiph.org/vorbis/', [vorbis]),
        ('components', 'Opus', opus_v, 'BSD-3-Clause', 'https://opus-codec.org', [opus]),
        ('components', 'opusfile', opusfile_v, 'BSD-3-Clause', 'https://opus-codec.org', [opusfile]),
        ('components', 'hterm', grab('deps/libapps/hterm/package.json', r'"version":\s*"([^"]+)"'),
         'BSD-3-Clause', 'https://chromium.googlesource.com/apps/libapps',
         ['deps/libapps/LICENSE']),
        ('components', 'wcwidth (in hterm)', '', 'BSD-3-Clause AND MIT',
         'https://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c',
         ['deps/libapps/libdot/third_party/wcwidth/LICENSE.md']),
        ('components', 'Intl.Segmenter polyfill (in hterm)', '', 'BSD-3-Clause',
         'https://chromium.googlesource.com/apps/libapps',
         ['deps/libapps/libdot/third_party/intl-segmenter/LICENSE.md']),
        ('components', 'Helix', '', 'MPL-2.0', 'https://helix-editor.com', ['deps/helix/LICENSE']),
        ('components', 'virglrenderer',
         grab('deps/virglrenderer/meson.build', r"version:\s*'([^']+)'"), 'MIT',
         'https://gitlab.freedesktop.org/virgl/virglrenderer', ['deps/virglrenderer/COPYING']),
        ('components', 'Venus protocol', venus.rsplit('-', 1)[-1] if venus_dirs else '', 'MIT',
         'https://gitlab.freedesktop.org/virgl/venus-protocol',
         [('Notice', first_comment(venus_header)), ('MIT License', read(f'{VENDORED}/MIT.txt'))]),
        ('components', 'MoltenVK', three('deps/MoltenVK/MoltenVK/MoltenVK/API/mvk_private_api.h', 'MVK_VERSION_')
         or three('deps/MoltenVK/MoltenVK/MoltenVK/API/vk_mvk_moltenvk.h', 'MVK_VERSION_'),
         'Apache-2.0', 'https://github.com/KhronosGroup/MoltenVK', ['deps/MoltenVK/LICENSE']),
        ('components', 'SPIRV-Cross (in MoltenVK)', '', 'Apache-2.0',
         'https://github.com/KhronosGroup/SPIRV-Cross',
         ['subprojects/MoltenVK/aok/external/SPIRV-Cross/LICENSE']),
        ('components', 'Vulkan-Headers (in MoltenVK)',
         grab('subprojects/MoltenVK/aok/external/Vulkan-Headers/include/vulkan/vulkan_core.h',
              r'#define VK_HEADER_VERSION (\d+)', lambda m: '1.4.' + m.group(1)),
         'Apache-2.0 OR MIT', 'https://github.com/KhronosGroup/Vulkan-Headers',
         ['subprojects/MoltenVK/aok/external/Vulkan-Headers/LICENSE.md',
          'subprojects/MoltenVK/aok/external/Vulkan-Headers/LICENSES/Apache-2.0.txt',
          'subprojects/MoltenVK/aok/external/Vulkan-Headers/LICENSES/MIT.txt']),
        ('components', 'cereal (in MoltenVK)',
         three('subprojects/MoltenVK/aok/external/cereal/include/cereal/version.hpp', 'CEREAL_VERSION_'),
         'BSD-3-Clause', 'https://uscilab.github.io/cereal/',
         ['subprojects/MoltenVK/aok/external/cereal/LICENSE']),
        ('components', 'wayvnc, neatvnc and aml (bundled for the Wayland desktop)',
         f"{bundled_source('wayvnc')}, {bundled_source('neatvnc')}, {bundled_source('aml')}", 'ISC',
         'https://github.com/any1/wayvnc',
         ['opt/AOK/bundled/devuan6-aarch64/wayvnc.COPYING']),
        ('components', 'wf-shell (wf-panel, bundled for the Wayland desktop)',
         read('opt/AOK/bundled/devuan6-aarch64/wf-panel.source').split()[0],
         'MIT AND GPL-2.0-or-later AND HPND-sell-variant', 'https://github.com/WayfireWM/wf-shell',
         [f'{VENDORED}/wf-shell.copyright', f'{VENDORED}/GPL-2.0.txt']),
        ('components', 'musl (in the bundled sshd)', '1.2.5', 'MIT', 'https://musl.libc.org',
         [f'{VENDORED}/musl-1.2.5.COPYRIGHT']),
    ]


# --- Rust ------------------------------------------------------------------

def xcconfig(key):
    m = re.search(rf'^{key}[ \t]*=[ \t]*([^\n]*?)[ \t]*$', read('app/iSH.xcconfig'), re.M)
    return m.group(1) if m else ''


def rust_features():
    # The same list xcode-meson.sh and meson.build hand cargo: AOK_RUST_FEATURES,
    # plus helix when AOK_NATIVE_HELIX is on.
    feats = [f for f in re.split(r'[ ,]+', xcconfig('AOK_RUST_FEATURES')) if f]
    if xcconfig('AOK_NATIVE_HELIX') == 'YES':
        feats.append('helix')
    return feats


def run(cmd, cwd):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if p.returncode != 0:
        die(f'{" ".join(cmd)} failed:\n{p.stderr.strip()[-2000:]}')
    return p.stdout


LICENCE_FILE = re.compile(r'^(LICEN[CS]E|COPYING|COPYRIGHT|NOTICE|UNLICENSE)', re.I)


def rust_crates():
    crate_dir = os.path.join(ROOT, 'deps', 'rust-native')
    feats = rust_features()
    common = ['--offline', '--locked', '--manifest-path', os.path.join(crate_dir, 'Cargo.toml')]
    if feats:
        common += ['--features', ','.join(feats)]
    tree = run(['cargo', 'tree', *common, '--target', 'aarch64-apple-ios', '-e', 'normal',
                '--prefix', 'none', '--format', '{p}'], crate_dir)
    wanted = set()
    for line in tree.splitlines():
        m = re.match(r'(\S+) v(\S+)', line.strip())
        if m:
            wanted.add((m.group(1), m.group(2)))
    meta = json.loads(run(['cargo', 'metadata', '--format-version', '1', *common,
                           '--filter-platform', 'aarch64-apple-ios'], crate_dir))
    root = next(p for p in meta['packages'] if p['id'] == meta['resolve']['root'])
    wanted.discard((root['name'], root['version']))  # AOK-Workstation's own code

    out = []
    for p in meta['packages']:
        key = (p['name'], p['version'])
        if key not in wanted:
            continue
        wanted.discard(key)
        d = os.path.dirname(p['manifest_path'])
        files = sorted(os.path.join(d, f) for f in os.listdir(d)
                       if LICENCE_FILE.match(f) and os.path.isfile(os.path.join(d, f)))
        if p.get('license_file'):
            lf = os.path.normpath(os.path.join(d, p['license_file']))
            if lf not in files and os.path.isfile(lf):
                files.append(lf)
        if not files and p['name'].startswith('helix-'):
            # helix's crates are path crates of the helix tree, whose licence is
            # at its root rather than in each crate.
            files = [os.path.join(ROOT, 'deps/helix/LICENSE')]
        vendored = os.path.join(ROOT, VENDORED, 'crates', p['name'] + '.LICENSE')
        if not files and os.path.isfile(vendored):
            files = [vendored]
        if not files:
            die(f'crate {p["name"]} {p["version"]} ({p.get("license")}) ships no licence file. '
                f'Fetch the LICENSE from its repository ({p.get("repository")}) at that '
                f'version into {VENDORED}/crates/{p["name"]}.LICENSE')
        url = p.get('repository') or p.get('homepage') or f'https://crates.io/crates/{p["name"]}'
        out.append(('crates', p['name'], p['version'], p.get('license') or '', url, files))
    if wanted:
        die('cargo tree lists crates cargo metadata does not: ' + ', '.join(f'{n} {v}' for n, v in sorted(wanted)))
    return out


def html_text(fragment):
    t = re.sub(r'<br\s*/?>|</p>|<p>|</div>|<div[^>]*>|</h[1-6]>|</pre>|</summary>', '\n', fragment)
    t = html.unescape(re.sub(r'<[^>]+>', '', t))
    t = '\n'.join(l.strip() for l in t.split('\n'))
    return re.sub(r'\n{3,}', '\n\n', t).strip()


def rust_std():
    sysroot = run(['rustc', '--print', 'sysroot'], ROOT).strip()
    version = rustc_version()
    doc = os.path.join(sysroot, 'share', 'doc', 'rust')
    page = read(os.path.join(doc, 'COPYRIGHT-library.html'))
    licences = os.path.join(doc, 'licenses')

    a = page.find('id="in-tree-files"')
    b = page.find('id="out-of-tree-dependencies"')
    if a < 0 or b < 0:
        die(f'{doc}/COPYRIGHT-library.html no longer has the sections this script reads')
    in_tree = html_text(page[page.find('>', a) + 1:b])
    in_tree = re.sub(r'^\s*In-tree files\s*', '', in_tree)
    named = sorted(set(re.findall(r'License:\s*(.+)', in_tree)))
    spdx = set(re.findall(r'[A-Za-z0-9.+-]+', ' '.join(named))) - {'OR', 'AND', 'WITH'}
    texts = [('Rust standard library: in-tree files', in_tree)]
    for f in sorted(os.listdir(licences)):
        if os.path.splitext(f)[0] in spdx:
            texts.append(os.path.join(licences, f))
    out = [('rust-std', 'Rust standard library', version, 'Apache-2.0 OR MIT',
            'https://www.rust-lang.org', texts)]

    rest = page[b:]
    for m in re.finditer(r'<h3>(.*?)</h3>(.*?)(?=<h3>|\Z)', rest, re.S):
        title = html.unescape(re.sub(r'<[^>]+>', '', m.group(1))).replace('📦', '').strip()
        name, _, ver = title.rpartition('-')
        if not name:
            name, ver = title, ''
        body = m.group(2)
        lic = re.search(r'<b>License:</b>\s*(.*?)</p>', body, re.S)
        url = re.search(r'<b>URL:</b>\s*<a href="([^"]+)"', body)
        notices = []
        for n in re.finditer(r'<summary><code>(.*?)</code></summary>\s*<pre>(.*?)</pre>', body, re.S):
            notices.append((html.unescape(n.group(1)), html.unescape(n.group(2)).strip('\n')))
        if not notices:
            # An entry with no notice file: what the page says is all there is.
            notices.append(('Notice', html_text(body)))
        out.append(('rust-std', name, ver, html.unescape(lic.group(1).strip()) if lic else '',
                    url.group(1) if url else '', notices))
    return out


# --- output ----------------------------------------------------------------

def build():
    entries = components() + rust_crates() + rust_std()
    texts, index = [], {}
    comps = []
    for group, name, version, licence, url, sources in entries:
        refs = []
        for s in sources:
            if isinstance(s, tuple):
                title, body = s
            else:
                title, body = os.path.basename(s), read(s)
            body = body.replace('\r\n', '\n').strip('\n')
            h = hashlib.sha1(body.encode()).hexdigest()
            if h not in index:
                index[h] = len(texts)
                texts.append(body)
            ref = [title, index[h]]
            if ref not in refs:
                refs.append(ref)
        comps.append({'group': group, 'name': name, 'version': version,
                      'licence': licence, 'url': url, 'texts': refs})
    order = {'app': 0, 'components': 1, 'crates': 2, 'rust-std': 3}
    # Each group alphabetical, except that the Rust standard library's own
    # entry heads the list of its dependencies.
    comps.sort(key=lambda c: (order[c['group']], c['name'] != 'Rust standard library',
                              c['name'].lower(), c['version']))
    return {
        'note': 'Generated by tools/gen-licences.py -- edit that, not this.',
        'groups': ['app', 'components', 'crates', 'rust-std'],
        'components': comps,
        'texts': texts,
    }


def render(data):
    # One component or text per line, so a dependency change is a readable diff.
    lines = ['{',
             f' "note": {json.dumps(data["note"])},',
             f' "groups": {json.dumps(data["groups"])},',
             ' "components": [']
    lines += ['  ' + json.dumps(c, ensure_ascii=False) + (',' if i < len(data['components']) - 1 else '')
              for i, c in enumerate(data['components'])]
    lines += [' ],', ' "texts": [']
    lines += ['  ' + json.dumps(t, ensure_ascii=False) + (',' if i < len(data['texts']) - 1 else '')
              for i, t in enumerate(data['texts'])]
    lines += [' ]', '}', '']
    return '\n'.join(lines)


def resolved(data, skip_group):
    """Every component outside skip_group with its texts spelled out, for comparing."""
    texts = data['texts']
    return [(c['group'], c['name'], c['version'], c['licence'], c['url'],
             [(t, texts[i]) for t, i in c['texts']])
            for c in data['components'] if c['group'] != skip_group]


def rustc_version():
    return re.match(r'rustc (\S+)', run(['rustc', '--version'], ROOT)).group(1)


def main():
    check = '--check' in sys.argv[1:]
    data = build()
    text = render(data)
    json.loads(text)  # the hand-rolled layout must still be JSON
    current = open(OUT, encoding='utf-8').read() if os.path.exists(OUT) else ''
    counts = {g: sum(1 for c in data['components'] if c['group'] == g) for g in data['groups']}
    summary = (f'{len(data["components"])} components ({", ".join(f"{g} {n}" for g, n in counts.items())}), '
               f'{len(data["texts"])} distinct texts, {len(text.encode()) // 1024} KiB')
    if check:
        if current == text:
            print(f'app/Licences.json is current: {summary}')
            return
        old = json.loads(current) if current else {'components': [], 'texts': []}
        if resolved(old, 'rust-std') != resolved(data, 'rust-std'):
            sys.exit(f'gen-licences: app/Licences.json is stale -- run tools/gen-licences.py ({summary})')
        # Only the Rust standard library differs, which is this machine's
        # toolchain rather than the tree: a release runner may well have a
        # different rustc from the Mac that last wrote the file.
        print(f'gen-licences: app/Licences.json matches the tree; only the Rust standard library '
              f'differs (rustc here is {rustc_version()}). Regenerate on the machine that builds '
              f'the release if that is not this one.')
        return
    if current != text:
        with open(OUT, 'w', encoding='utf-8') as f:
            f.write(text)
        print(f'wrote app/Licences.json: {summary}')
    else:
        print(f'app/Licences.json unchanged: {summary}')


if __name__ == '__main__':
    main()
