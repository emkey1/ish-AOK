# Crypto acceleration

Emulating a cipher instruction by instruction is slow. iSH-AOK can run one
host-native instead: the guest asks the emulator to do the work through a
private syscall, and the emulator runs it at full host speed. On an A10X iPad
that takes ChaCha20 from about 8.8 MB/s to about 19 MB/s.

**It only pays off on an optimized build of iSH-AOK.** At -O0 the host cipher
runs 7-14x slower and the accelerator loses to plain emulation; see the
measurements below. `uname -v` reports " unoptimized" on such a build.

Nothing uses it automatically. Two pieces have to be in place:

1. **The toggle**, in iSH-AOK Settings, called *Enable Crypto Accel*. This
   controls the syscall itself. Off by default.
2. **The OpenSSL provider**, a small shared library installed *inside your root
   filesystem*, which is what actually routes OpenSSL's ciphers to the syscall.

The second one is the part people miss. The toggle on its own accelerates
nothing, because nothing in the guest is asking. If you have the toggle on and
see no change, you almost certainly have not installed the provider.

## Install

```sh
sudo sh /AOK/tools/crypto/install-crypto-accel.sh
```

That builds the provider from source in your root, installs it where OpenSSL
looks for modules, wires it into `openssl.cnf`, and then reports which
algorithms it took over and how they time with and without it. If it lists no
algorithms, the provider loaded but declined, which almost always means the
Settings toggle is off.

It needs a C compiler and the OpenSSL development headers:

```sh
apk add build-base openssl-dev          # Alpine
apt install build-essential libssl-dev  # Debian, Devuan, Ubuntu
pacman -S base-devel openssl            # Arch
```

Useful flags: `--dry-run` shows the exact diff it would apply and changes
nothing, `--uninstall` reverses everything, `-v` narrates each step.

Long-running programs read `openssl.cnf` when they start, so restart anything
already running. For ssh that means restarting `sshd` if you want existing
listeners to accelerate new connections.

## Verify

```sh
openssl list -providers
```

Both `default` and `ish` should be listed. Then ask which algorithms it
actually took over, which is the reliable check:

```sh
openssl list -cipher-algorithms | grep '@ ish'
```

You should see `ChaCha20` and `AES-256-GCM`. An empty list means the provider
loaded and declined everything: check the Settings toggle.

You can also compare timings:

```sh
openssl speed -evp aes-256-gcm -seconds 3
OPENSSL_CONF=/dev/null openssl speed -evp aes-256-gcm -seconds 3
```

but treat those numbers as a rough hint only. `openssl speed` measures guest
CPU time, and CPU-time accounting under emulation is not accurate enough to
trust for small differences.

To confirm ssh specifically is picking it up, look for the module inside a live
session process:

```sh
grep -l ish.so /proc/*/maps
```

## What is accelerated

| Algorithm | Accelerated | Used by |
|---|---|---|
| AES-256-GCM | yes | TLS (https), ssh with `-c aes256-gcm@openssh.com` |
| ChaCha20 (raw stream) | yes | ssh, scp, sftp |
| ChaCha20-Poly1305 (AEAD) | no, see below | TLS (https) |
| Everything else | no | disk tools |

The two are probed independently, because the host can have the instructions
for one and not the other, and neither is ever withheld on account of the
other.

ssh's default is the case ChaCha20 pays off for, because OpenSSH's
`chacha20-poly1305@openssh.com` computes its stream with libcrypto's raw
ChaCha20 and does the Poly1305 authentication in its own code. The raw stream
is the accelerated half.

**AES-256-GCM is accelerated as a whole AEAD**, which is what https uses. It
covers the three call sequences real consumers drive an AEAD through: the
generic one that TLS 1.3 uses, OpenSSH's, and TLS 1.2's (which asks the cipher
to handle the record framing itself). Anything outside those fails loudly
rather than producing wrong bytes -- notably a message larger than 4 MiB or
more than 4096 bytes of associated data in one operation, neither of which any
TLS or ssh record comes close to, and a tag shorter than the full 16 bytes.

**ChaCha20-Poly1305 as a single AEAD is deliberately not registered.** The
implementation does not reproduce the call sequence OpenSSL's TLS record layer
uses, and it fails with a bad record MAC rather than declining cleanly, so
registering it broke every TLS 1.3 connection that negotiated
`TLS_CHACHA20_POLY1305_SHA256`. A provider is chosen by property and cannot
fall back once chosen, so there is no safe way to offer it partially. TLS keeps
using OpenSSL's own implementation and works normally.

## Which guests

Only **arm64** and **riscv64**. On i386 and amd64 guests the syscall raises
SIGSYS by design and the installer refuses to run: those guests already execute
these ciphers at reasonable speed, so there is nothing to win.

## It is safe to leave installed

The provider declines any algorithm it cannot accelerate, and `openssl.cnf`
requests it as a preference (`?provider=ish`) rather than a requirement, so
OpenSSL silently uses its own code for everything else. If the toggle is off,
the self-test fails and the provider declines everything, which is simply the
unaccelerated behaviour.

The installer treats `openssl.cnf` carefully, because breaking it breaks every
OpenSSL program on the system including ssh. It never edits the live file: it
builds a candidate, checks that the candidate still has a working default
provider, RNG and digests, and only then moves it into place, keeping a backup
at `openssl.cnf.ish-accel.bak` and restoring it automatically if the installed
file fails the same checks.

One detail worth knowing if you ever hand-edit that file: in an OpenSSL config,
a section declared a second time **replaces** the first rather than merging into
it. Appending a `[provider_sect]` that lists only `ish` therefore silently drops
the default provider and takes the RNG down with it. The stock config warns
about this in a comment, and the installer edits the existing sections in place
to avoid it.

## Measurements, and the build type that dominates them

The accelerator's throughput is set almost entirely by how the **emulator** was
compiled. Same source, same hardware, one syscall per 16 KiB record:

| emulator build | AES-256-GCM | ChaCha20 |
|---|---|---|
| `-O0` (meson's default; was the Xcode Debug default) | 53.6 MB/s | 97.8 MB/s |
| `-O2` (`debugoptimized`, what ships) | **770.8 MB/s** | **708.5 MB/s** |

14x and 7x. The cost is per-byte and perfectly linear (a 256 KiB request runs
at the same MB/s as a 1 KiB one) and a zero-length call costs 1-2 us, so this
is not syscall overhead and batching would not help. It is the host cipher
itself, compiled without optimization.

For scale, guests emulate these ciphers at roughly 8.9 MB/s (ChaCha20 on an
A10X), 22.5 (AES-256-GCM, A10X), and 126 / 255 on an M4. An optimized
accelerator at ~700-770 MB/s is far above all of them; an -O0 one is below
most.

### On an -O0 build it is a net loss

Two devices running -O0 builds, 64 MiB `scp`, cipher pinned, median of 3:

| cipher | A10X stock | A10X accel | M4 stock | M4 accel |
|---|---|---|---|---|
| chacha20-poly1305 | 4.11 | **5.53** (1.35x) | **32.36** | 29.82 (0.92x) |
| aes256-gcm | **9.58** | 9.31 (0.97x) | **50.72** | 28.07 (0.55x) |

Kept here as a warning, not as a description of a shipping build: at -O0 the
accelerator's ceiling falls below what the guest already manages, so enabling
it makes transfers slower. If your numbers look like this, check `uname -v`
for " unoptimized" before drawing any conclusion about the accelerator.

### Choosing a cipher

Independently of the accelerator, `aes256-gcm@openssh.com` beat
`chacha20-poly1305` on both devices tested, and OpenSSH prefers ChaCha20 by
default. If throughput is what you care about:

```sh
scp -c aes256-gcm@openssh.com bigfile user@host:/path
```

or put `Ciphers aes256-gcm@openssh.com` in `~/.ssh/config`.

Measure your own hardware rather than trusting these figures: they are two
iPads over two links.

## Files

| Path | What |
|---|---|
| `/AOK/tools/crypto/install-crypto-accel.sh` | installer, uninstaller |
| `/AOK/tools/crypto/ish_provider.c` | provider source, built in-guest |
| `/AOK/tools/crypto/README.md` | implementation notes |
| `<modulesdir>/ish.so` | installed module |
| `<openssldir>/openssl.cnf` | edited, backup alongside |

In the iSH-AOK source tree these live under `opt/AOK/tools/crypto/`, and the
emulator side is `kernel/ish_accel.c` with `kernel/ish_accel_crypto.c`.
