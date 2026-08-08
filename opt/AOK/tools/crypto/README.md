# iSH-AOK crypto-accelerator OpenSSL provider

Routes OpenSSL's `AES-256-GCM` and `ChaCha20` through the iSH-AOK crypto
accelerator syscall (ISH_SYS_AEAD / kernel/ish_accel.c) so they run host-native
instead of being emulated instruction by instruction.

- **AES-256-GCM** is the whole AEAD, so it covers https and
  `ssh -c aes256-gcm@openssh.com`.
- **ChaCha20** is the raw stream, which is what OpenSSH's default
  `chacha20-poly1305@openssh.com` uses (via cipher-chachapoly-libcrypto).

Either way ssh/scp and TLS accelerate transparently, with no changes to them.

Requires the accelerator enabled (`ISH_CRYPTO_ACCEL=1`, or the app's
*Enable Crypto Accel* toggle). The two algorithms are probed independently,
matching the kernel's independent self-tests: a host with the AES instructions
but no ChaCha support, or the reverse, gets whichever works. Anything the
accelerator declines is simply not registered, and OpenSSL falls back to its
own code, so the provider is always safe to load.

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
- AES-256-GCM, arm64 guest on an Apple-silicon **host** (CLI emulator): 234
  MB/s vs 128 MB/s at 16 KiB (1.8x), 120 vs 73 at 4 KiB, roughly par at 1 KiB.
  One accelerator call costs ~1.7 us there, less than `getpid`.

**That last figure does not transfer to devices, and on-device AES-256-GCM
acceleration is a loss.** The syscall round trip is far more expensive on iOS
than on the CLI, and the emulated cipher is faster on recent silicon, so the
two cross over: measured on hardware, accelerated AES-256-GCM `scp` runs at
0.97x on an A10X and 0.55x on an M4. `/AOK/docs/crypto-accel.md` has the full
two-device table and the rule that predicts the sign. Treat CLI numbers as a
correctness and mechanism check only; benchmark decisions on a device.

**Benchmark on an optimized build only.** The kernel's AES is NEON intrinsics,
so an `-O0` emulator build runs it at about 23 MB/s and the accelerator comes
out slower still. That is a debug-build artifact on top of everything above.
`openssl speed` is also a poor instrument here: it times with guest CPU-time
accounting, which is unreliable under emulation (and `-elapsed` divides by a
zero interval). Measure wall-clock throughput directly, or trust
`openssl list -cipher-algorithms | grep '@ ish'` for the yes/no question of
whether the provider took the algorithm on.

## Scope / limitations

- **AES-256-GCM** covers the three call sequences real consumers use: the
  generic one (also TLS 1.3's, re-initialising per record with just an IV),
  OpenSSH's (`SET_IV_FIXED` once, then `IV_GEN` per packet with everything
  through `EVP_Cipher`), and TLS 1.2's (`AEAD_TLS1_AAD` returning a pad, then
  one in-place update covering explicit-IV||payload||tag). Every consumer sets
  the tag before the data on decrypt, which is what lets the accelerator's
  one-shot op serve all three. Outside those: a second data update in one
  record, more than 4096 bytes of AAD, a message over 4 MiB, a tag shorter
  than 16 bytes, or a nonce that is not 96 bits all fail loudly. None of them
  can be folded onto a one-shot syscall that cannot resume GCM state, and a
  provider chosen by property cannot fall back, so failing is the only safe
  answer. No TLS or ssh record comes close to those caps.
- **ChaCha20 is the raw stream only.** ChaCha20-Poly1305 is implemented
  (`cp_*`) but deliberately **not registered**: OpenSSL's TLS record layer
  drives the AEAD through a call sequence it does not reproduce, and it fails
  with a bad record MAC instead of declining, which broke every TLS 1.3
  connection negotiating `TLS_CHACHA20_POLY1305_SHA256`. Build with
  `-DISH_PROVIDER_ENABLE_AEAD` to re-register it while working on that surface;
  the gate to reinstate it is what the AES code now does and its gates prove.
  The raw stream is correct for the one-init + one-cipher pattern ssh uses and
  for 64-byte-aligned streaming updates; a mid-stream sub-block-straddling
  update poisons the ctx, so the caller errors rather than getting wrong data.
- Must be built per guest arch inside the rootfs, which is what the installer
  does. arm64 and riscv64 guests only; the syscall raises SIGSYS on x86 guests
  by design.

## Validating a change to the AES path

The ChaCha20-Poly1305 AEAD shipped broken because its checks shared the
one-shot call pattern with the implementation, so nothing exercised what TLS
actually does. Re-run all of these against a real consumer, many records each:

1. `openssl list -cipher-algorithms | grep '@ ish'` shows AES-256-GCM, and
   wall-clock throughput beats the default provider.
2. A random differential against the default provider: random keys, IVs, AAD
   and plaintext lengths, split AAD spans, in-place operation, zero-length
   plaintext, plus the negative cases above asserting loud failure.
3. TLS 1.3: `s_server`/`s_client` pinned to `TLS_AES_256_GCM_SHA384`, at least
   1 MiB each way, and mixed with a stock peer to prove wire interop.
4. TLS 1.2 (`-tls1_2`), which is the only thing that exercises the `tlsaad`
   path -- and if that ctrl fails, the record layer errors out and https dies.
5. `curl` to a real https site.
6. In-guest `sshd` with `scp -c aes256-gcm@openssh.com`, a large file both
   ways, for thousands of packet cycles through `tlsivgen`.
7. ChaCha20's own delta is still there and a TLS chacha20-poly1305 handshake
   still succeeds.

A passing handshake alone proves nothing: `?provider=ish` is a preference, so
OpenSSL will quietly use the default provider if the fetch does not match.
Confirm the traffic really went through this code -- for example by building a
throwaway copy of the provider that counts accelerator calls -- before
believing a green run.
