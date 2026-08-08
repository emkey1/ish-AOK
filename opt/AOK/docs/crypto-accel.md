# Crypto acceleration

Emulating a cipher instruction by instruction is slow. iSH-AOK can run one
host-native instead: the guest asks the emulator to do the work through a
private syscall, and the emulator runs it at full host speed. On an A10X iPad
that takes ChaCha20 from about 8.8 MB/s to about 19 MB/s.

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
looks for modules, wires it into `openssl.cnf`, and then proves it worked by
timing ChaCha20 with and without it. If the two timings come back equal, the
provider loaded but declined, which almost always means the Settings toggle is
off.

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

Both `default` and `ish` should be listed. Then compare directly:

```sh
openssl speed -evp chacha20 -seconds 3
```

against the same command with the provider bypassed:

```sh
OPENSSL_CONF=/dev/null openssl speed -evp chacha20 -seconds 3
```

To confirm ssh specifically is picking it up, look for the module inside a live
session process:

```sh
grep -l ish.so /proc/*/maps
```

## What is accelerated

| Algorithm | Accelerated | Used by |
|---|---|---|
| ChaCha20 (raw stream) | yes | ssh, scp, sftp |
| ChaCha20-Poly1305 (AEAD) | no, see below | TLS (https) |
| AES, everything else | no | TLS, disk tools |

ssh is the case that pays off, because OpenSSH's default cipher,
`chacha20-poly1305@openssh.com`, computes its stream with libcrypto's raw
ChaCha20 and does the Poly1305 authentication in its own code. The raw stream
is the accelerated half.

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

## Worth measuring before you rely on it

On an A10X iPad, pushing a 64 MiB file with `scp`:

| cipher | accelerated | MB/s |
|---|---|---|
| chacha20-poly1305 | no | 3.47 |
| chacha20-poly1305 | yes | 4.70 |
| aes256-gcm | no | 8.07 |

The accelerator is a real gain for ssh, but on that hardware **AES-256-GCM is
faster than accelerated ChaCha20 even with no accelerator at all**, because the
guest advertises the ARMv8 crypto instructions and the JIT maps them
efficiently. OpenSSH prefers ChaCha20 by default, so the stock configuration
picks the slower option. If throughput is what you care about:

```sh
scp -c aes256-gcm@openssh.com bigfile user@host:/path
```

or put `Ciphers aes256-gcm@openssh.com` in `~/.ssh/config`. Measure on your own
device before assuming either result carries over: the numbers above are one
ten-year-old iPad over one link, and the transfer was partly limited by the
link itself (about 14.9 MB/s with no encryption at all).

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
