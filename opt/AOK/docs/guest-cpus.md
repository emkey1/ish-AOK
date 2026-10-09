# What each guest CPU tells your programs

Every guest in iSH-AOK is emulated, so the CPU a program sees is the one
iSH-AOK describes: through `CPUID` on x86, `AT_HWCAP` and the ID registers on
arm64, `riscv_hwprobe` on riscv64, and `/proc/cpuinfo` on all four. Software
reads that description to pick its fast paths — glibc's `memcpy`, OpenSSL's
ciphers, a codec's inner loop — so what it says matters as much as what the
emulator can do. The rule is simple: a feature is reported only when every
instruction behind it runs, as gadgets in the JIT, and the guest suite runs an
instruction for every bit it sets.

To see what your root is being told:

```sh
grep -m1 -E '^(flags|Features|isa)' /proc/cpuinfo   # one line, whichever guest you are
uname -r                                            # 5.10.0-ish_aok: the kernel iSH-AOK reports
```

## x86: i386 and amd64

As of 558 both x86 guests report the vector units: AVX, AVX2, FMA and F16C,
BMI1/2, MOVBE and LZCNT, AES-NI, PCLMULQDQ, VAES, VPCLMULQDQ and GFNI,
AVX-VNNI, and the AVX-512 families — F, DQ, CD, BW, VL, IFMA, VBMI, VBMI2,
VNNI, BITALG, VPOPCNTDQ and BF16 — with XSAVE behind them. An amd64 guest's
`flags` line reads, in part:

```
flags : ... sse4_2 movbe popcnt aes xsave osxsave avx f16c ... avx2 bmi2 avx512f avx512dq ... umip ... rdpid
```

That makes the amd64 guest an **x86-64-v4** machine. A glibc root says so
itself:

```sh
/lib64/ld-linux-x86-64.so.2 --help | grep x86-64-v   # v4, v3 and v2: "supported, searched"
```

so a distribution or a binary built for x86-64-v3 runs, and glibc picks its
AVX2 and EVEX string functions on its own.

Things worth knowing:

- **The vector registers survive everything a kernel does to them.** A signal
  frame carries Linux's XSAVE image (with its magic numbers, so a handler can
  find the `ymm`/`zmm` state), `sigreturn` restores it by Linux's rules,
  `ptrace` serves it as `NT_X86_XSTATE` for gdb, and `exec` starts clean. That
  was the last thing standing between the instructions and the `CPUID` bits.
- **The CPU is family 6, model 0, on purpose.** No real part has that model, so
  software that tunes on the model number — glibc's ifunc choices, HotSpot's
  JCC-erratum padding, `gcc -march=native` — finds nothing to key on and goes by
  the feature bits instead. `model name` reads `iSH Virtual x86_64-compatible
  CPU`.
- **`SGDT`, `SIDT`, `SLDT`, `STR` and `SMSW` answer as on a UMIP processor**
  under Linux 5.10, which spoofs them; `umip` is in the flags, and the kernel log
  gets Linux's ratelimited line when a program uses one. `LAR`, `LSL`, `VERR`
  and `VERW` answer from Linux's own descriptor table. All nine used to be
  `SIGILL`.
- **`RDTSCP` and `RDPID` work**, reporting the CPU that `getcpu()` reports.
- **The MMX registers are the x87 registers**, as on the hardware: code that
  mixes the two, or a signal handler that uses MMX, sees what real silicon shows.
- **x87 arithmetic is exact to the hardware**, the transcendentals included
  (correctly rounded, which an AMD Ryzen itself is not in about 2% of cases).
- **None of the vector bits are set by the CLI built on an x86_64 Linux host.**
  That backend runs the interpreter, so it keeps them dark; the app, and the CLI
  on a Mac with Apple silicon, report them.

## arm64

The arm64 guest reports what an ARMv8 core with the crypto extensions has —
`fp asimd cpuid aes pmull sha1 sha2 crc32 atomics sha3 sha512` — plus **MOPS**,
ARMv8.8's memory-copy and memory-set instructions, which no Apple chip has.
glibc 2.41 turns `memcpy`, `memmove` and `memset` into a single MOPS
instruction when it sees the bit, and to a gadget JIT that is one dispatch
instead of a NEON loop of dozens per 64 bytes. To compare with and without it:

```sh
echo 0 > /proc/ish/arm64_mops   # hide MOPS from programs started from now on
echo 1 > /proc/ish/arm64_mops   # and show it again
```

`ISH_MOPS=0` does the same from the start in the CLI. A process that already
chose MOPS keeps working either way; the instructions always execute.

## riscv64

As of 558 the riscv64 guest is an **RVA23** machine, the profile Ubuntu's
riscv64 builds require from 25.10: the scalar extensions (Zba, Zbb, Zbs,
Zicond, Zcb, Zfa, Zfhmin, Zicbo*, Zimop/Zcmop, Zawrs) and the vector unit, V
with Zvbb, Zvkb, Zvkt and Zvfhmin. The `isa` line starts
`rv64imafdcv_...`, `AT_HWCAP` has `v`, and `riscv_hwprobe` answers as Linux
does. Pointer masking (Supm) is the one piece left out. The details, and
iSH-AOK's hook for non-standard vendor instructions, are in
[riscv64-vendor-extensions.md](riscv64-vendor-extensions.md).

## See also

- [tuning-knobs.md](tuning-knobs.md) — `ISH_GUEST_CPU_COUNT`, and how many CPUs
  the guest believes it has.
- [benchmarks.md](benchmarks.md) — measuring what a fast path is worth.
- [proc-ish.md](proc-ish.md) — the rest of `/proc/ish`.
