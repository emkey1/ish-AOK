# Vendored licence texts

`tools/gen-licences.py` builds `app/Licences.json`, which the app's Licences
screen shows. Most texts come straight from the trees the build compiles
(submodules, `subprojects/`, crates in the Cargo cache, the Rust toolchain).
These are the ones that are not in any of those, so they are kept here:

| File | Why it is here |
| --- | --- |
| `GPL-3.0.txt`, `GPL-2.0.txt` | AOK-iSH's own licence (LICENSE.md names both and links to them), and wf-shell's GPL-2.0-or-later part. GPL-3.0 is GNU's text as in deps/bash/COPYING; GPL-2.0 is GNU's text as XZ Utils ships it. |
| `libogg-1.3.5.COPYING`, `libvorbis-1.3.7.COPYING`, `opus-1.5.2.COPYING`, `opusfile-0.12.COPYING` | `deps/audiocodecs-static` is a prebuilt xcframework; its build script downloads the sources and keeps none. From github.com/xiph at those tags. The generator refuses to run if the build script names another version. |
| `xz.COPYING`, `xz.COPYING.0BSD` | `deps/liblzma-static/src` vendors liblzma's sources without them (XZ Utils 5.8). |
| `musl-1.2.5.COPYRIGHT` | The bundled guest sshd (`tools/guest-sshd-aarch64.tar.lzma`) is linked statically against the musl zig 0.17 carries. |
| `wf-shell.copyright` | The bundled `wf-panel` is Devuan's wf-shell build; this is its `/usr/share/doc/wf-shell/copyright`. |
| `MIT.txt` | The MIT permission text, for a notice that gives the copyright line but not the text (the Venus protocol's headers). |
| `crates/<name>.LICENSE` | Crates whose published package leaves its licence file out. From each crate's repository at the published version. |

When the generator stops with "ships no licence file", add that crate's
LICENSE here under `crates/`, from its repository at the version in use.
