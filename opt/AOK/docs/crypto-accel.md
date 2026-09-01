# Crypto acceleration

Emulating a cipher instruction by instruction is slow. iSH-AOK can run one
host-native instead: the guest asks the emulator to do the work through a
private syscall, and the emulator runs it at full host speed. On an A10X iPad
that takes ChaCha20 from about 8.8 MB/s to about 19 MB/s.

**Check you are on an optimized build first.** At -O0 the host cipher runs
7-17x slower and the accelerator loses to plain emulation; `uname -v` reports
" unoptimized" when that is the case. On a normal build it is a solid win, 1.15x
to 1.86x on real transfers depending on the device and cipher.

Nothing uses it automatically. Two pieces have to be in place:

1. **The toggle**, in iSH-AOK Settings, called *Crypto Accel (ssh,
   arm64/riscv64)*. This controls the syscall itself. Off by default.
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

**This applies to your distro's ssh, not to the native one.** iSH-AOK's native
`ssh`, `scp`, `sftp` and `ssh-keygen` -- reached as `/AOK/native/smallclue ssh`,
or as plain `ssh` once `/AOK/tools/native-links.sh` has run -- are host code
compiled into the app, and they are built **without OpenSSL**:

```sh
$ /AOK/native/smallclue ssh -V
OpenSSH_10.2p1, without OpenSSL
```

They use their own bundled crypto, so an OpenSSL provider installed in the guest
cannot reach them and nothing here accelerates them. Everything below describes
guest programs that link the guest's OpenSSL. See
[native-programs.md](native-programs.md) for what runs natively and how to tell.

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

"Accelerated" here means "routed through the syscall", not "made faster" --
which of these is actually a win depends on the device, and often it is not.

| Algorithm | Routed through the accelerator | Used by |
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

Only **arm64** and **riscv64** — as a matter of policy, not capability. The
accelerator syscall is wired for every guest ABI, so a provider probing for it
on i386 or amd64 now gets a clean refusal rather than `SIGSYS`; the installer
still declines to run there, because those guests already execute these ciphers
at reasonable speed and there is nothing to win.

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

## Measurements

Two devices, optimized builds, 64 MiB `scp`, cipher pinned, two sshds differing
only in `OPENSSL_CONF`, arms interleaved, median of 3.

| cipher | A10X stock | A10X accel | | M4 stock | M4 accel | |
|---|---|---|---|---|---|---|
| chacha20-poly1305 | 4.16 | **7.75** | 1.86x | 36.13 | **47.67** | 1.32x |
| aes256-gcm | 10.32 | **18.85** | 1.83x | 60.32 | **69.59** | 1.15x |

A10X = iPad Pro 12.9" 2nd gen over Lightning, M4 = iPad Pro 11" over USB-C.
Fastest configuration on both is **aes256-gcm with the accelerator on**.

The M4's gains are smaller only because its transfers are closer to
link-and-protocol bound at 60-70 MB/s, not because the accelerator is weaker
there; its ceiling is the highest of anything measured here.

Cipher rates, EVP at 16 KiB records:

| | A10X stock | A10X accel | M4 stock | M4 accel |
|---|---|---|---|---|
| chacha20 | 9.20 | **135.9** (14.8x) | 130.9 | **548.2** (4.2x) |
| aes-256-gcm | 23.29 | **98.3** (4.2x) | 279.8 | **448.8** (1.6x) |
| aes-128-gcm (not accelerated) | 26.76 | 26.74 | 309.9 | 309.7 |

### Why the build type matters so much

Raw accelerator throughput, one syscall per 16 KiB record:

| | A10X -O0 | A10X -O2 | M4 -O0 | M4 -O2 |
|---|---|---|---|---|
| aes-256-gcm | 24.3 | **411.6** | 54.0 | **901.0** |
| chacha20 | 21.9 | **282.1** | 98.7 | **703.9** |

13-17x, from the emulator's optimization level alone. Syscall dispatch is only
1-2 us and the per-byte cost is linear out to 256 KiB, so essentially all of
that is the host cipher being compiled without optimization.

The trap is that the guest's *emulated* rates barely move between the two
builds (A10X ChaCha20 8.88 -> 9.20), because guest crypto runs as JIT-generated
gadget sequences and is largely insulated from the C core's optimization level.
So a debug build does not scale both sides down: it drags only the accelerator,
and **inverts** the comparison. On -O0 these same two devices measured 0.55x to
1.35x, i.e. the accelerator looked like a net loss. If you see numbers like
that, check `uname -v` before concluding anything.

### Choosing a cipher

OpenSSH prefers `chacha20-poly1305` by default, but `aes256-gcm@openssh.com`
was faster on both devices, accelerated or not:

```sh
scp -c aes256-gcm@openssh.com bigfile user@host:/path
```

or put `Ciphers aes256-gcm@openssh.com` in `~/.ssh/config`.

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
