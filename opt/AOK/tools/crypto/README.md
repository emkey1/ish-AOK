# iSH-AOK crypto-accelerator OpenSSL provider

Routes OpenSSL's `ChaCha20` stream cipher through the iSH-AOK crypto
accelerator syscall (ISH_SYS_AEAD / kernel/ish_accel.c) so it runs host-native
instead of being emulated instruction by instruction. This is the cipher
OpenSSH's default `chacha20-poly1305@openssh.com` uses (via
cipher-chachapoly-libcrypto), so loading this provider accelerates ssh/scp
transparently, with no changes to ssh.

Requires the accelerator enabled (`ISH_CRYPTO_ACCEL=1`, or the app's
*Enable Crypto Accel* toggle). If unavailable the provider declines the
algorithm and OpenSSL falls back to its own ChaCha20, so it is always safe to
load.

User-facing setup instructions live in [/AOK/docs/crypto-accel.md](../../docs/crypto-accel.md).

## Install

```sh
sudo sh install-crypto-accel.sh      # build, install, wire up openssl.cnf, verify
sudo sh install-crypto-accel.sh --dry-run
sudo sh install-crypto-accel.sh --uninstall
```

`build-provider.sh` is the older minimal path: it builds the module and prints
a config snippet for you to merge by hand. Prefer the installer, which edits
the existing config sections in place and validates the result before it takes
effect.

## Measured

Release/-O2 build, 16 KiB records:

- riscv64: `EVP_chacha20` via ish provider ~496 MB/s vs ~26 MB/s default (~19x);
  transparent via openssl.cnf ~15x; output bit-identical to OpenSSL's default
  provider (200-case differential)
- arm64 on A10X (iPad Pro 2nd gen): 19.1 MB/s vs 8.8 MB/s (2.2x), which moves
  a 64 MiB `scp` from 3.47 to 4.70 MB/s (1.36x). The rest of that transfer is
  emulated Poly1305 plus protocol overhead, neither of which this touches.

## Scope / limitations

- **Raw ChaCha20 stream only.** ChaCha20-Poly1305 is implemented (`cp_*`) but
  deliberately **not registered**: OpenSSL's TLS record layer drives the AEAD
  through a call sequence it does not reproduce, and it fails with a bad record
  MAC instead of declining, which broke every TLS 1.3 connection negotiating
  `TLS_CHACHA20_POLY1305_SHA256`. A provider cannot fall back once selected by
  property, so partial support is not safe to offer. Build with
  `-DISH_PROVIDER_ENABLE_AEAD` to re-register it while working on that surface;
  the gate to reinstate it is a passing `s_client` handshake.
- Correct for the one-init + one-cipher pattern ssh uses and 64-byte-aligned
  streaming updates; a mid-stream sub-block-straddling update poisons the ctx
  (the accelerator declines, so the caller errors rather than getting wrong
  data).
- Must be built per guest arch inside the rootfs, which is what the installer
  does. arm64 and riscv64 guests only; the syscall raises SIGSYS on x86 guests
  by design.
- AES is not accelerated. On ARMv8-capable guests, un-accelerated AES-256-GCM
  currently outruns accelerated ChaCha20 anyway, so an AES-GCM accelerator is
  the higher-value follow-on.
