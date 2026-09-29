#!/usr/bin/env python3
"""Report on an ISH_JIT_PROFILE file (jit/jitprof.c): the dynamic instruction
mix of arm64 and riscv64 guest code, how long the runs of SIMD/FP instructions
are, and the commonest adjacent pairs -- the numbers for deciding between
fusing gadgets and translating runs natively.

Every guest instruction costs about the same under the gadget JIT, so the
dynamic instruction count is also where the time goes.

    python3 tools/jitprof-report.py PROFILE [--mc llvm-mc] [--top N]

Mnemonics come from llvm-mc (LLVM's, e.g. Homebrew's /opt/homebrew/opt/llvm/bin;
Apple's toolchain has no llvm-mc). Without it the report still gives the mix by
encoding class, and names pairs by class.
"""

import argparse
import collections
import os
import shutil
import subprocess
import sys

MC_TRIPLES = {
    "arm64": ["-triple=aarch64"],
    "riscv64": ["-triple=riscv64", "-mattr=+m,+a,+f,+d,+c,+zba,+zbb,+zbs,+zicsr,+zifencei"],
}


def find_mc(explicit):
    for cand in [explicit, shutil.which("llvm-mc"), "/opt/homebrew/opt/llvm/bin/llvm-mc",
                 "/usr/local/opt/llvm/bin/llvm-mc"]:
        if cand and os.path.exists(cand):
            return cand
    return None


def disassemble(mc, abi, words):
    """{word: 'mnemonic operands'} for the words llvm-mc can decode."""
    if mc is None:
        return {}
    lines = []
    for w in words:
        b = w.to_bytes(4, "little")
        lines.append(" ".join("0x%02x" % x for x in b))
    out = subprocess.run([mc, "--disassemble", "--show-encoding"] + MC_TRIPLES[abi],
                         input="\n".join(lines), capture_output=True, text=True).stdout
    names = {}
    for line in out.splitlines():
        if "encoding: [" not in line:
            continue
        text, enc = line.split("encoding: [", 1)
        text = text.rstrip().rstrip("/#").strip()
        bs = [int(x, 16) for x in enc.split("]")[0].split(",")]
        if len(bs) == 4:
            names[int.from_bytes(bytes(bs), "little")] = " ".join(text.split())
    return names


# ---- encoding classes -------------------------------------------------------

def arm64_class(w):
    op0 = (w >> 25) & 0xF
    if op0 in (8, 9):
        return "dp-imm"
    if op0 in (10, 11):
        return "branch/sys"
    if op0 in (4, 6, 12, 14):
        return "simd-ldst" if (w >> 26) & 1 else "ldst"
    if op0 in (5, 13):
        return "dp-reg"
    if op0 in (7, 15):
        # Advanced SIMD vector forms have bit 28 clear; scalar FP and scalar
        # SIMD have it set.
        return "simd-vector" if not (w >> 28) & 1 else "fp-scalar"
    return "other"


def riscv64_class(w):
    opcode = w & 0x7F
    return {
        0x03: "ldst", 0x23: "ldst", 0x07: "fp-ldst", 0x27: "fp-ldst",
        0x13: "dp-imm", 0x1B: "dp-imm", 0x37: "dp-imm", 0x17: "dp-imm",
        0x33: "dp-reg", 0x3B: "dp-reg",
        0x63: "branch/sys", 0x67: "branch/sys", 0x6F: "branch/sys", 0x73: "branch/sys", 0x0F: "branch/sys",
        0x53: "fp-scalar", 0x43: "fp-scalar", 0x47: "fp-scalar", 0x4B: "fp-scalar", 0x4F: "fp-scalar",
        0x2F: "atomic", 0x57: "simd-vector",
    }.get(opcode, "other")


CLASSIFY = {"arm64": arm64_class, "riscv64": riscv64_class}
SIMD_FP = {"simd-vector", "simd-ldst", "fp-scalar", "fp-ldst"}
# Register-only data processing: what a native translation of a straight run
# could cover without touching the TLB or leaving the block.
REG_DP = {"dp-imm", "dp-reg", "simd-vector", "fp-scalar"}


def run_histogram(title, runlen, total):
    covered = sum(runlen.values())
    if not covered:
        return
    print("\n" + title)
    for lim in (1, 2, 3, 4, 6, 8, 12, 16, 32, 10**9):
        n = sum(v for k, v in runlen.items() if k <= lim)
        label = "<= %d" % lim if lim < 10**9 else "all"
        print("  %-6s %6.2f%% cumulative   (%5.2f%% of all instructions)" % (
            label, 100.0 * n / covered, 100.0 * n / total))
    # dispatches saved if every run of >= 2 became one dispatch
    saved = sum(v - v // k for k, v in runlen.items() if k >= 2)
    print("  share of all instructions: %.2f%%; one dispatch per run of 2+ would save %.1f%% of all dispatches" % (
        100.0 * covered / total, 100.0 * saved / total))


def mnemonic(names, w, cls):
    n = names.get(w)
    return n.split()[0] if n else "<%s>" % cls


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("profile")
    ap.add_argument("--mc")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()
    mc = find_mc(args.mc)

    blocks = collections.defaultdict(list)
    with open(args.profile) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 4:
                continue
            blocks[parts[0]].append((int(parts[1]), [int(x, 16) for x in parts[3:]]))

    for abi, recs in sorted(blocks.items()):
        classify = CLASSIFY[abi]
        words = sorted({w for _, insns in recs for w in insns})
        names = disassemble(mc, abi, words)
        total = 0
        by_class = collections.Counter()
        by_mnem = collections.Counter()
        pairs = collections.Counter()
        simd_pairs = collections.Counter()
        runlen = collections.Counter()      # run length -> dynamic SIMD/FP instructions in such runs
        dplen = collections.Counter()       # the same for register-only data processing
        block_len = collections.Counter()   # instructions per block, weighted by executions
        for count, insns in recs:
            total += count * len(insns)
            block_len[len(insns)] += count * len(insns)
            classes = [classify(w) for w in insns]
            mn = [mnemonic(names, w, c) for w, c in zip(insns, classes)]
            for c, m in zip(classes, mn):
                by_class[c] += count
                by_mnem[m] += count
            for i in range(len(insns) - 1):
                pairs[(mn[i], mn[i + 1])] += count
                if classes[i] in SIMD_FP or classes[i + 1] in SIMD_FP:
                    simd_pairs[(mn[i], mn[i + 1])] += count
            for member, hist in ((SIMD_FP, runlen), (REG_DP, dplen)):
                run = 0
                for c in classes + ["end"]:
                    if c in member:
                        run += 1
                    elif run:
                        hist[run] += run * count
                        run = 0

        print("=" * 72)
        print("%s: %d blocks ran, %.1fM dynamic instructions%s" % (
            abi, len(recs), total / 1e6, "" if names else "  (no llvm-mc: classes only)"))
        print("\nby encoding class")
        for c, n in by_class.most_common():
            print("  %-12s %6.2f%%" % (c, 100.0 * n / total))
        mean_block = sum(n * c for n, c in block_len.items()) / max(1, total)
        print("\ninstructions per block entered (weighted by instructions): mean %.1f" % mean_block)
        simd_total = sum(runlen.values())
        run_histogram("SIMD/FP instructions by the length of the straight run they sit in", runlen, total)
        run_histogram("register-only data processing (scalar or vector, no memory, no branch) by run length",
                      dplen, total)
        print("\ntop %d mnemonics" % args.top)
        for m, n in by_mnem.most_common(args.top):
            print("  %-14s %6.2f%%" % (m, 100.0 * n / total))
        print("\ntop %d adjacent pairs" % args.top)
        for (a, b), n in pairs.most_common(args.top):
            print("  %-12s -> %-12s %6.2f%%" % (a, b, 100.0 * n / total))
        if simd_total:
            print("\ntop %d adjacent pairs touching SIMD/FP" % args.top)
            for (a, b), n in simd_pairs.most_common(args.top):
                print("  %-12s -> %-12s %6.2f%%" % (a, b, 100.0 * n / total))
        print()


if __name__ == "__main__":
    sys.exit(main())
