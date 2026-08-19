#!/usr/bin/env python3
"""
conductor.py — iSH-AOK differential / crash-resilient test conductor.

Builds each corpus test three ways (i386 ELF, x86_64 ELF, x86_64 macOS Mach-O
oracle), runs every (arch x engine) cell plus external x86 oracle(s), then does a
KEY-BASED comparison (not whole-file diff, so an i386 cell may legitimately omit
64-bit lines). Divergences and crashes are reported and minimized.

Oracles / cells:
  oracle           native x86_64 via `arch -x86_64` (Rosetta Mach-O) — x86_64 truth
  mint:x86_64      x86_64 ELF in mint's Lima Linux VM    — real-Linux x86_64 truth
  mint:i386        i386   ELF in mint's Lima Linux VM    — the i386 truth the M5 lacks
  amd64:interp     ish, /proc/ish/amd64_jit=0
  amd64:jit        ish, /proc/ish/amd64_jit=1
  i386:jit         ish, default i386 JIT
  i386:no_cache    ish, /proc/ish/i386_no_cache_comm=<comm>
  i386:single_step ish, /proc/ish/i386_single_step_comm=<comm>

mint cells are auto-skipped when mint is unreachable, so the M5 runs standalone.
Static ELFs run in the VM via the read-only virtiofs mount of mint's home — a
plain `scp` to mint makes them instantly visible+executable inside the VM.

Backends:
  local-fakefs   the host ./build/ish process IS the device; a JIT bug that kills
                 it surfaces as a signal exit -> CRASH.
  ssh / devicectl  iOS-device backend (journal + heartbeat) — see README, not built.

Config via env: ISH_MINT_HOST (mint), ISH_LIMA_INSTANCE (ish), ISH_MINT_BINDIR
(.ish-oracle/bin, relative to mint's home).

Usage:
  python3 conductor.py run [--tests a,b] [--cells ...] [--seed N] [--no-mint]
  python3 conductor.py minimize --test T --cell amd64:jit [--seed N]
"""
import argparse, json, os, re, shlex, shutil, subprocess, sys, tarfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
ISH = REPO / "build" / "ish"
FAKEFSIFY = REPO / "build" / "tools" / "fakefsify"
CORPUS = HERE / "corpus"
CONFORM = HERE / "corpus_signal"           # signal/syscall conformance vs REAL LINUX (mint)
CORPUS_FS = HERE / "corpus_fs"             # filesystem/VFS conformance vs REAL LINUX (mint)
CORPUS_PROC = HERE / "corpus_proc"         # process/thread lifecycle conformance vs REAL LINUX (mint)
CORPUS_TIME = HERE / "corpus_time"         # time/clock/timer conformance vs REAL LINUX (mint)
CORPUS_SOCK = HERE / "corpus_sock"         # socket/IPC conformance vs REAL LINUX (mint)
CORPUS_MEM = HERE / "corpus_mem"           # memory-management conformance vs REAL LINUX (mint)
CONFORM_DIRS = [CONFORM, CORPUS_FS, CORPUS_PROC, CORPUS_TIME, CORPUS_SOCK, CORPUS_MEM]  # all conformance corpora (same machinery, real-Linux oracle)
TESTS_MANUAL = REPO / "tests" / "manual"   # Tier 0: the self-checking regression suite
WORK = HERE / ".work"
TIMEOUT = 180  # seconds per cell; exceed => HANG

ZIG_TARGET = {"i386": "x86-linux-musl", "x86_64": "x86_64-linux-musl"}

# cell -> dict(arch, engine-spec, kind). kind: rosetta | ish | mint
CELLS = {
    "oracle":           dict(arch="x86_64", engine="oracle",           kind="rosetta"),
    "mint:x86_64":      dict(arch="x86_64", engine="oracle",           kind="mint"),
    "mint:i386":        dict(arch="i386",   engine="oracle",           kind="mint"),
    "amd64:interp":     dict(arch="x86_64", engine="amd64:interp",     kind="ish"),
    "amd64:jit":        dict(arch="x86_64", engine="amd64:jit",        kind="ish"),
    "i386:jit":         dict(arch="i386",   engine="i386:jit",         kind="ish"),
    "i386:no_cache":    dict(arch="i386",   engine="i386:no_cache",    kind="ish"),
    "i386:single_step": dict(arch="i386",   engine="i386:single_step", kind="ish"),
    # device cells run the guest supervisor on a real device over ssh:1022 (see
    # the `device` subcommand). Excluded from DEFAULT_CELLS — they need a device.
    "device:amd64:jit":    dict(arch="x86_64", engine="amd64:jit",    kind="device"),
    "device:amd64:interp": dict(arch="x86_64", engine="amd64:interp", kind="device"),
    "device:i386:jit":     dict(arch="i386",   engine="i386:jit",     kind="device"),
    # iSH built in mint's x86_64 Linux VM: a different i386 JIT codegen
    # (gadgets-x86_64) than the M5/device aarch64 gadgets -> independent cell.
    # Needs the VM ish built (ISH_MINT_ISH); opt-in via --cells, not in defaults.
    "mint:i386:jit":       dict(arch="i386",   engine="i386:jit",     kind="mintish"),
}
ORACLE_KINDS = {"rosetta", "mint"}
DEFAULT_CELLS = [c for c, s in CELLS.items() if s["kind"] not in ("device", "mintish")]

def discover_tests():
    return sorted(p.stem for p in CORPUS.glob("*.c"))

def sh(cmd, timeout=None):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


# ---------------------------------------------------------------- mint backend

def probe_mint():
    """Return mint config dict if reachable with a runnable Lima instance, else None."""
    host = os.environ.get("ISH_MINT_HOST", "mint")
    inst = os.environ.get("ISH_LIMA_INSTANCE", "ish")
    rb = os.environ.get("ISH_MINT_BINDIR", ".ish-oracle/bin")
    base = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", host]
    try:
        home = sh(base + ["pwd"], timeout=15).stdout.strip()
        if not home:
            return None
        chk = sh(base + ["bash", "-lc", f"limactl shell {shlex.quote(inst)} -- true"], timeout=30)
        if chk.returncode != 0:
            print(f"  mint up but lima '{inst}' not runnable — skipping mint cells", file=sys.stderr)
            return None
    except subprocess.TimeoutExpired:
        return None
    return dict(host=host, instance=inst, base=base,
                remote_bindir=rb, vm_bindir=f"{home}/{rb}",
                vm_ish=os.environ.get("ISH_MINT_ISH", "/tmp/ish-item4/build/ish"),
                vm_fsdir=os.environ.get("ISH_MINT_FSDIR", "/tmp/ish-mint-fs"))

def push_to_mint(tests, binmap, mint):
    host = mint["host"]
    sh(mint["base"] + ["mkdir", "-p", mint["remote_bindir"]], timeout=20)
    files = [str(binmap[t][a]) for t in tests for a in ZIG_TARGET]
    r = sh(["scp", "-p", "-o", "BatchMode=yes", *files, f"{host}:{mint['remote_bindir']}/"], timeout=60)
    if r.returncode != 0:
        raise SystemExit(f"scp to mint failed:\n{r.stderr}")

def mint_run_cmd(mint, test, arch, extra):
    vmbin = f"{mint['vm_bindir']}/{test}.{arch}"
    inner_args = " ".join(shlex.quote(x) for x in [vmbin, "--engine", "oracle", *extra])
    inner = f"limactl shell {shlex.quote(mint['instance'])} -- {inner_args}"
    remote = "bash -lc " + shlex.quote(inner)
    return ["ssh", "-o", "BatchMode=yes", mint["host"], remote]

def stage_mint_ish(mint):
    """Copy the pushed binaries into a writable VM dir so the VM's ish can `-r`
    it (the ~/.ish-oracle mount is read-only). Needed for the mint:i386:jit cell,
    which runs the corpus under iSH built in the mint VM (x86_64-host i386 JIT)."""
    inner = (f"mkdir -p {mint['vm_fsdir']}/bin && "
             f"cp {mint['vm_bindir']}/* {mint['vm_fsdir']}/bin/ 2>/dev/null; "
             f"chmod +x {mint['vm_fsdir']}/bin/* 2>/dev/null; true")
    remote = "bash -lc " + shlex.quote(
        f"limactl shell {shlex.quote(mint['instance'])} -- sh -c {shlex.quote(inner)}")
    sh(["ssh", "-o", "BatchMode=yes", mint["host"], remote], timeout=60)

def mint_ish_run_cmd(mint, test, arch, engine, extra):
    inner_args = " ".join(shlex.quote(x) for x in
        [mint["vm_ish"], "-r", mint["vm_fsdir"], f"/bin/{test}.{arch}",
         "--engine", engine, *extra])
    inner = f"limactl shell {shlex.quote(mint['instance'])} -- {inner_args}"
    remote = "bash -lc " + shlex.quote(inner)
    return ["ssh", "-o", "BatchMode=yes", mint["host"], remote]


# ---------------------------------------------------------------- build

def build_variants(test):
    src = CORPUS / f"{test}.c"
    out = {}
    for arch, triple in ZIG_TARGET.items():
        dst = WORK / "bin" / f"{test}.{arch}"
        r = sh(["zig", "cc", "-target", triple, "-static", "-O2",
                "-I", str(CORPUS), "-o", str(dst), str(src)])
        if r.returncode != 0:
            raise SystemExit(f"build {test}.{arch} failed:\n{r.stderr}")
        out[arch] = dst
    oracle = WORK / "bin" / f"{test}.oracle"
    r = sh(["cc", "-arch", "x86_64", "-O2", "-I", str(CORPUS), "-o", str(oracle), str(src)])
    if r.returncode != 0:
        raise SystemExit(f"build {test}.oracle failed:\n{r.stderr}")
    out["oracle"] = oracle
    return out

SUPERVISOR_SRC = HERE / "guest_supervisor.c"

def build_supervisor():
    """Build the in-guest batch runner for both arches (static)."""
    for arch, triple in ZIG_TARGET.items():
        dst = WORK / "bin" / f"guest_supervisor.{arch}"
        r = sh(["zig", "cc", "-target", triple, "-static", "-O2",
                "-o", str(dst), str(SUPERVISOR_SRC)])
        if r.returncode != 0:
            raise SystemExit(f"build supervisor {arch} failed:\n{r.stderr}")

def build_fakefs(tests):
    stage = WORK / "stage"
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "bin").mkdir(parents=True)
    for test in tests:
        for arch in ZIG_TARGET:
            shutil.copy(WORK / "bin" / f"{test}.{arch}", stage / "bin" / f"{test}.{arch}")
    for arch in ZIG_TARGET:                       # include the supervisor if built
        sup = WORK / "bin" / f"guest_supervisor.{arch}"
        if sup.exists():
            shutil.copy(sup, stage / "bin" / sup.name)
    tar = WORK / "fs.tar.gz"
    with tarfile.open(tar, "w:gz") as t:
        t.add(stage, arcname=".")
    fs = WORK / "fs"
    if fs.exists():
        shutil.rmtree(fs)
    r = sh([str(FAKEFSIFY), str(tar), str(fs)])
    if r.returncode != 0:
        raise SystemExit(f"fakefsify failed:\n{r.stderr}")
    return fs


# ------------------------------------------ conformance corpus (vs real Linux)
#
# Signal/syscall *semantics* are validated against a REAL LINUX kernel (mint's
# Lima VM), not Rosetta: a macOS Mach-O has Darwin signal semantics and lacks
# Linux-only APIs (signalfd, rt signals), so it is the wrong oracle. These tests
# therefore build ONLY the two Linux musl ELFs (no macOS oracle build) and the
# `conform` subcommand compares the iSH cells against mint:x86_64 / mint:i386.

def discover_conform():
    seen = []
    for d in CONFORM_DIRS:
        if d.exists():
            seen += [p.stem for p in d.glob("*.c")]
    return sorted(seen)

def conform_src(test):
    """Locate a conformance test's source across the corpora (signal + fs)."""
    for d in CONFORM_DIRS:
        p = d / f"{test}.c"
        if p.exists():
            return p
    raise SystemExit(f"no conformance test named {test} in {[str(d) for d in CONFORM_DIRS]}")

def build_conform_variants(test):
    """Build i386 + x86_64 static musl ELFs (no macOS oracle — see above)."""
    src = conform_src(test)
    includes = []
    for d in CONFORM_DIRS:
        includes += ["-I", str(d)]
    out = {}
    for arch, triple in ZIG_TARGET.items():
        dst = WORK / "bin" / f"{test}.{arch}"
        r = sh(["zig", "cc", "-target", triple, "-static", "-O2", "-pthread",
                *includes, "-o", str(dst), str(src)])
        if r.returncode != 0:
            raise SystemExit(f"build {test}.{arch} failed:\n{r.stderr}")
        out[arch] = dst
    return out

def build_conform_fakefs(tests):
    """Stage the conformance binaries plus a /bin/true (per arch) and a writable
    /tmp into a fakefs. fork/exec-based tests need a real child binary."""
    stage = WORK / "stage_conform"
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "bin").mkdir(parents=True)
    (stage / "tmp").mkdir()
    true_c = WORK / "true.c"
    true_c.write_text("int main(void){return 0;}\n")
    for arch, triple in ZIG_TARGET.items():
        sh(["zig", "cc", "-target", triple, "-static", "-O2",
            "-o", str(stage / "bin" / f"true.{arch}"), str(true_c)])
    shutil.copy(stage / "bin" / "true.x86_64", stage / "bin" / "true")
    for test in tests:
        for arch in ZIG_TARGET:
            shutil.copy(WORK / "bin" / f"{test}.{arch}", stage / "bin" / f"{test}.{arch}")
    tar = WORK / "conform.tar.gz"
    with tarfile.open(tar, "w:gz") as t:
        t.add(stage, arcname=".")
    fs = WORK / "conformfs"
    if fs.exists():
        shutil.rmtree(fs)
    r = sh([str(FAKEFSIFY), str(tar), str(fs)])
    if r.returncode != 0:
        raise SystemExit(f"conform fakefsify failed:\n{r.stderr}")
    return fs


# ------------------------------------------------ Tier 0: tests/manual suite

def discover_tier0():
    """The self-checking tests/manual tests (exit 0 + '<name>: PASS')."""
    return sorted(p.stem for p in TESTS_MANUAL.glob("*.c")
                  if '#include "test_common.h"' in p.read_text())

def build_tier0(tests):
    """Build each self-check test (static, -pthread) + a /bin/true into a per-arch
    staging dir. Returns {arch: [tests that built]} (some are arch-specific)."""
    built = {a: [] for a in ZIG_TARGET}
    true_c = WORK / "true.c"
    true_c.write_text("int main(void){return 0;}\n")
    for arch, triple in ZIG_TARGET.items():
        root = WORK / "tier0" / arch
        if root.exists():
            shutil.rmtree(root)
        (root / "bin").mkdir(parents=True)
        (root / "tmp").mkdir()   # writable scratch some tests need
        sh(["zig", "cc", "-target", triple, "-static", "-O2",
            "-o", str(root / "bin" / "true"), str(true_c)])
        for t in tests:
            r = sh(["zig", "cc", "-target", triple, "-static", "-O2", "-pthread",
                    "-I", str(TESTS_MANUAL), "-o", str(root / "bin" / t),
                    str(TESTS_MANUAL / f"{t}.c")])
            if r.returncode == 0:
                built[arch].append(t)
    return built

def tier0_fakefs(arch):
    tar = WORK / f"tier0-{arch}.tgz"
    with tarfile.open(tar, "w:gz") as t:
        t.add(WORK / "tier0" / arch, arcname=".")
    fs = WORK / f"tier0fs-{arch}"
    if fs.exists():
        shutil.rmtree(fs)
    r = sh([str(FAKEFSIFY), str(tar), str(fs)])
    if r.returncode != 0:
        raise SystemExit(f"tier0 fakefsify {arch} failed:\n{r.stderr}")
    return fs

def run_tier0(test, fakefs):
    """A self-check test passes iff it exits 0 AND prints '<test>: PASS'.

    SKIP is its own verdict, not a failure. A test that says it cannot run here
    -- no accelerator in this build, no second mount, no peer binaries -- exits
    0 and prints '<test>: SKIP ...'. Counting that as FAIL made a completely
    clean tier0 report "4 failed" on both arches, which is exactly the kind of
    standing noise that stops anyone reading the output.
    """
    try:
        p = subprocess.run([str(ISH), "-f", str(fakefs), f"/bin/{test}"],
                           capture_output=True, text=True, timeout=TIMEOUT)
        if p.returncode == 0:
            if re.search(rf"(?m)^{re.escape(test)}: PASS(?:$|[^A-Za-z])", p.stdout):
                return "PASS", p.returncode, p.stdout + p.stderr
            if re.search(rf"(?m)^{re.escape(test)}: SKIP(?:$|[^A-Za-z])", p.stdout):
                return "SKIP", p.returncode, p.stdout + p.stderr
        return "FAIL", p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return "HANG", None, ""


# ---------------------------------------------------------------- run a cell

class Result:
    def __init__(self, cell, status, rc, lines, stderr, secs):
        self.cell, self.status, self.rc = cell, status, rc
        self.lines, self.stderr, self.secs = lines, stderr, secs

def classify(rc, timed_out):
    if timed_out:
        return "HANG"
    if rc is not None and rc < 0:
        return "CRASH"          # killed by signal -> emulator/host death
    if rc == 0:
        return "OK"
    return "TEST_ERR"

def run_cell(cell, test, binaries, fakefs, mint=None, extra=None):
    spec = CELLS[cell]
    arch, engine, kind = spec["arch"], spec["engine"], spec["kind"]
    extra = extra or []
    t0 = time.time()
    timed_out = False
    if kind == "rosetta":
        cmd = ["arch", "-x86_64", str(binaries["oracle"]), "--engine", "oracle", *extra]
    elif kind == "mint":
        cmd = mint_run_cmd(mint, test, arch, extra)
    elif kind == "mintish":
        cmd = mint_ish_run_cmd(mint, test, arch, engine, extra)
    else:  # ish
        cmd = [str(ISH), "-f", str(fakefs), f"/bin/{test}.{arch}", "--engine", engine, *extra]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT)
        rc, out, err = p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        rc, out, err, timed_out = None, (e.stdout or b"").decode("utf8", "replace"), "", True
    return Result(cell, classify(rc, timed_out), rc, out.splitlines(), err, time.time() - t0)


# ------------------------------------------------ supervised (device) batch run

def parse_supervisor_stream(text):
    """Parse the SUPER-START/<output>/SUPER-END journal into per-test
    (output_lines, rc). A SUPER-START with no matching SUPER-END means the
    emulator died (or was killed on timeout) during that test -- it is the
    crasher, returned as `dangling`. This is exactly what survives on a real
    device: the app crash drops the ssh stream, but the fsync'd journal still
    shows the dangling START."""
    tests = {}
    cur = None
    for ln in text.splitlines():
        if ln.startswith("SUPER-START "):
            p = ln.split(maxsplit=2)
            cur = {"idx": p[1], "name": p[2] if len(p) > 2 else "?", "out": []}
        elif ln.startswith("SUPER-END ") and cur is not None:
            p = ln.split()
            if len(p) >= 3 and p[1] == cur["idx"]:
                tests[cur["name"]] = (cur["out"], int(p[2]))
                cur = None
        elif cur is not None:
            cur["out"].append(ln)
    return tests, (cur["name"] if cur else None)

def run_supervised(cell, tests, binmap, fakefs, seed=1, timeout=TIMEOUT):
    """Run the whole batch under the guest supervisor in one ish launch (the
    device model). Returns ({test: Result}, crasher_name, seconds)."""
    arch, engine = CELLS[cell]["arch"], CELLS[cell]["engine"]
    cmd = [str(ISH), "-f", str(fakefs), f"/bin/guest_supervisor.{arch}",
           "--engine", engine, "--seed", str(seed), "--journal", "/journal",
           *[f"/bin/{t}.{arch}" for t in tests]]
    t0 = time.time(); timed_out = False
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out = p.stdout
    except subprocess.TimeoutExpired as e:
        out, timed_out = (e.stdout or b"").decode("utf8", "replace"), True
    secs = time.time() - t0
    parsed, dangling = parse_supervisor_stream(out)
    results = {}
    for t in tests:
        key = f"{t}.{arch}"   # the supervisor journals binaries by basename
        if key in parsed:
            lines, trc = parsed[key]
            st = "OK" if trc == 0 else ("CRASH" if trc < 0 else "TEST_ERR")
            results[t] = Result(cell, st, trc, lines, "", secs / max(len(tests), 1))
        elif key == dangling:
            results[t] = Result(cell, "HANG" if timed_out else "CRASH", None, [],
                                "supervisor journal truncated at this test", secs)
        else:
            results[t] = Result(cell, "SKIP", None, [], "not run (after crasher)", 0.0)
    return results, dangling, secs


# ------------------------------------------------ device backend (ssh + recovery)
#
# UNVALIDATED SCAFFOLD (no live device yet). Mirrors the proven mint ssh backend
# for transport (deploy/run/retrieve) and reuses parse_supervisor_stream for
# crash reconciliation; the crash-detection + recovery loop is new. `--dry-run`
# prints every ssh/scp/devicectl command without executing, so the shapes can be
# reviewed without a device. The device IP varies, so --device-host is required.

def device_cfg(args):
    d = (args.device_dir or "/tmp/ish-remote").rstrip("/")
    return {"host": args.device_host, "port": str(args.device_port),
            "user": args.device_user, "dir": d, "journal": d + "/journal",
            "recover": args.recover, "udid": args.device_udid,
            "bundle": args.device_bundle, "dry": args.dry_run}

def _ssh_base(cfg):
    return ["ssh", "-p", cfg["port"], "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=8", f"{cfg['user']}@{cfg['host']}"]

def device_run(cfg, remote_cmd, timeout=60):
    """Run a command in the guest over ssh:1022. Returns the CompletedProcess, a
    TimeoutExpired (caller treats as a possible crash), or None on --dry-run."""
    if cfg["dry"]:
        print("  DRY ssh:", remote_cmd)
        return None
    try:
        return subprocess.run(_ssh_base(cfg) + [remote_cmd],
                              capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        return e

def device_scp(cfg, files, dest):
    if cfg["dry"]:
        print(f"  DRY scp: {len(files)} file(s) -> {cfg['host']}:{dest}")
        return None
    cmd = ["scp", "-P", cfg["port"], "-o", "BatchMode=yes",
           *[str(f) for f in files], f"{cfg['user']}@{cfg['host']}:{dest}"]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=120)

def device_up(cfg):
    if cfg["dry"]:
        return True
    import socket
    try:
        with socket.create_connection((cfg["host"], int(cfg["port"])), timeout=5):
            return True
    except OSError:
        return False

def wait_for_device(cfg, max_wait=300):
    if cfg["dry"]:
        return True
    deadline = time.time() + max_wait
    while time.time() < deadline:
        if device_up(cfg):
            time.sleep(2)   # let sshd settle after relaunch
            return True
        time.sleep(3)
    return False

def device_deploy(cfg, tests, binmap):
    device_run(cfg, f"mkdir -p {cfg['dir']}/bin")
    files = [binmap[t][a] for t in tests for a in ZIG_TARGET]
    files += [WORK / "bin" / f"guest_supervisor.{a}" for a in ZIG_TARGET]
    device_scp(cfg, files, f"{cfg['dir']}/bin/")
    device_run(cfg, f"chmod +x {cfg['dir']}/bin/*")

def device_supervisor_cmd(cfg, cell, tests):
    arch, engine = CELLS[cell]["arch"], CELLS[cell]["engine"]
    bins = " ".join(f"{cfg['dir']}/bin/{t}.{arch}" for t in tests)
    return (f"{cfg['dir']}/bin/guest_supervisor.{arch} --engine {engine} "
            f"--seed %d --journal {cfg['journal']} {bins}")

def device_recover(cfg):
    """Relaunch the app after a crash. devicectl for a USB-tethered device;
    otherwise notify the operator and poll for the app to return."""
    if cfg["recover"] == "devicectl":
        cmd = ["xcrun", "devicectl", "device", "process", "launch",
               "--terminate-existing", "--device", cfg["udid"] or "<udid>", cfg["bundle"]]
        if cfg["dry"]:
            print("  DRY recover:", " ".join(cmd))
        else:
            sh(cmd, timeout=90)
    elif cfg["recover"] == "notify":
        print(f"\n  *** DEVICE CRASHED — relaunch iSH-AOK on the device; "
              f"waiting for ssh:{cfg['port']} to return ***")
    else:
        return False
    return wait_for_device(cfg)

def device_run_batch(cfg, cell, tests, seed, timeout=900):
    """Supervised batch on the device with crash recovery. The supervisor journals
    each test; if the app crashes mid-batch the ssh stream is cut and the port
    goes down -- after relaunch the fsync'd fs journal's dangling SUPER-START
    names the crasher. Quarantine it, recover, and resume after it."""
    arch = CELLS[cell]["arch"]
    results, remaining = {}, list(tests)
    while remaining:
        r = device_run(cfg, device_supervisor_cmd(cfg, cell, remaining) % seed, timeout=timeout)
        if cfg["dry"]:
            return {t: Result(cell, "SKIP", None, [], "dry run", 0.0) for t in tests}
        stream = "" if (r is None or isinstance(r, subprocess.TimeoutExpired)) else r.stdout
        parsed, dangling = parse_supervisor_stream(stream)
        for t in list(remaining):
            key = f"{t}.{arch}"
            if key in parsed:
                lines, rc = parsed[key]
                st = "OK" if rc == 0 else ("CRASH" if rc < 0 else "TEST_ERR")
                results[t] = Result(cell, st, rc, lines, "", 0.0)
        remaining = [t for t in remaining if t not in results]
        if dangling is None:
            break                       # batch completed
        if device_up(cfg):
            continue                    # transient disconnect; rerun the rest
        # confirmed app crash: relaunch, then trust the fsync'd fs journal
        print(f"    device down — recovering ({cfg['recover']})")
        if not device_recover(cfg):
            break
        jr = device_run(cfg, f"cat {cfg['journal']}", timeout=30)
        if jr is not None and not isinstance(jr, subprocess.TimeoutExpired):
            _, j_dangling = parse_supervisor_stream(jr.stdout)
            dangling = j_dangling or dangling
        crasher = next((t for t in remaining if f"{t}.{arch}" == dangling), None)
        if crasher:
            results[crasher] = Result(cell, "CRASH", None, [], "crashed the app on device", 0.0)
            remaining = [t for t in remaining if t != crasher]
        print(f"    crasher: {crasher or dangling}; resuming {len(remaining)} test(s)")
    for t in tests:
        results.setdefault(t, Result(cell, "SKIP", None, [], "not run", 0.0))
    return results


# ---------------------------------------------------------------- compare

def parse(lines):
    table = {}
    for ln in lines:
        toks = ln.split()
        if not toks or not any(t.startswith("res=") or t.startswith("fl=") for t in toks):
            continue
        key = " ".join(t for t in toks if not (t.startswith("res=") or t.startswith("fl=")))
        val = " ".join(t for t in toks if t.startswith("res=") or t.startswith("fl="))
        table[key] = val
    return table

def compare(results):
    tables = {r.cell: parse(r.lines) for r in results if r.status == "OK"}
    all_keys = set().union(*tables.values()) if tables else set()
    mismatches = []
    for key in sorted(all_keys):
        vals = {c: t[key] for c, t in tables.items() if key in t}
        if len(set(vals.values())) > 1:
            mismatches.append((key, vals))
    return mismatches, tables

def oracle_value(vals):
    """Pick a ground-truth value for a key: prefer Rosetta, then mint."""
    for c in ("oracle", "mint:x86_64", "mint:i386"):
        if c in vals:
            return c, vals[c]
    return None, None


# ---------------------------------------------------------------- minimize

def minimize_crash(cell, test, binaries, fakefs):
    full = run_cell(cell, test, binaries, fakefs)
    if full.status not in ("CRASH", "HANG"):
        print("  full run did not crash; nothing to minimize.")
        return None
    r = sh(["arch", "-x86_64", str(binaries["oracle"]), "--list"])
    n = int(re.search(r"cases=(\d+)", r.stdout).group(1))
    print(f"  scanning {n} cases for the crasher in {cell} ...")
    for i in range(n):
        rr = run_cell(cell, test, binaries, fakefs, extra=["--case", str(i)])
        if rr.status in ("CRASH", "HANG"):
            print(f"  minimal crashing case: --case {i}  (status {rr.status})")
            return i
    print("  no single case reproduced the crash (state-dependent).")
    return None


# ---------------------------------------------------------------- report

def report(test, results, mismatches):
    print(f"\n=== {test} ===")
    for r in results:
        flag = {"OK": "ok", "CRASH": "CRASH", "HANG": "HANG", "TEST_ERR": "ERR",
                "SKIP": "skip"}[r.status]
        n = len(parse(r.lines)) if r.status == "OK" else 0
        oracle_tag = " (oracle)" if CELLS[r.cell]["kind"] in ORACLE_KINDS else ""
        print(f"  [{flag:5}] {r.cell:16}{oracle_tag:9} rc={r.rc} {n:5} cases {r.secs:6.2f}s")
        if r.status != "OK" and r.stderr.strip():
            print("    stderr:", r.stderr.strip().splitlines()[-1][:120])
    if not mismatches:
        print("  → all cells agree ✓")
        return
    print(f"  → {len(mismatches)} divergent keys")
    by_op = {}
    for key, _ in mismatches:
        by_op[key.split()[0]] = by_op.get(key.split()[0], 0) + 1
    print("    by op:", ", ".join(f"{k}={v}" for k, v in sorted(by_op.items(), key=lambda x: -x[1])))
    for key, vals in mismatches[:8]:
        oc, ov = oracle_value(vals)
        diffs = {c: v for c, v in vals.items() if v != ov and c not in ORACLE_CELLS}
        print(f"    {key}: {oc}[{ov}] vs " + "; ".join(f"{c}[{v}]" for c, v in diffs.items()))
    if len(mismatches) > 8:
        print(f"    ... +{len(mismatches)-8} more")

ORACLE_CELLS = {c for c, s in CELLS.items() if s["kind"] in ORACLE_KINDS}


# ---------------------------------------------------------------- main

def cmd_run(args):
    tests = args.tests.split(",") if args.tests else discover_tests()
    cells = args.cells.split(",") if args.cells else DEFAULT_CELLS[:]
    WORK.mkdir(exist_ok=True)
    (WORK / "bin").mkdir(exist_ok=True)

    mint = None if args.no_mint else probe_mint()
    if any(CELLS[c]["kind"] in ("mint", "mintish") for c in cells):
        if mint:
            print(f"mint: {mint['host']} lima/{mint['instance']} → {mint['vm_bindir']}")
        else:
            print("mint unavailable — running M5-local cells only")
            cells = [c for c in cells if CELLS[c]["kind"] not in ("mint", "mintish")]

    print(f"building {len(tests)} test(s): {', '.join(tests)}")
    binmap = {t: build_variants(t) for t in tests}
    fakefs = build_fakefs(tests)
    if mint and any(CELLS[c]["kind"] in ("mint", "mintish") for c in cells):
        push_to_mint(tests, binmap, mint)
    if mint and any(CELLS[c]["kind"] == "mintish" for c in cells):
        stage_mint_ish(mint)

    summary, any_fail = {}, False
    for test in tests:
        results = [run_cell(c, test, binmap[test], fakefs, mint=mint,
                            extra=["--seed", str(args.seed)]) for c in cells]
        mism, _ = compare(results)
        report(test, results, mism)
        bad = [r.cell for r in results if r.status in ("CRASH", "HANG")]
        if bad or mism:
            any_fail = True
        summary[test] = {
            "cells": {r.cell: {"status": r.status, "rc": r.rc,
                               "cases": len(parse(r.lines)), "secs": round(r.secs, 2)}
                      for r in results},
            "divergent_keys": len(mism),
            "crashed_cells": bad,
        }
    (WORK / "results.json").write_text(json.dumps(summary, indent=2))
    print(f"\nwrote {WORK / 'results.json'}")
    sys.exit(1 if any_fail else 0)

def cmd_minimize(args):
    WORK.mkdir(exist_ok=True)
    (WORK / "bin").mkdir(exist_ok=True)
    binmap = build_variants(args.test)
    fakefs = build_fakefs([args.test])
    minimize_crash(args.cell, args.test, binmap, fakefs)

def cmd_supervise(args):
    """Device-model run: ish cells go through the guest supervisor (one launch,
    journaled) so a crash that drops the connection is still attributable; the
    oracle/mint cells run per-test as usual. Run locally it validates the same
    comparison and exercises the journal/crash-reconciliation path."""
    tests = args.tests.split(",") if args.tests else discover_tests()
    cells = args.cells.split(",") if args.cells else DEFAULT_CELLS[:]
    WORK.mkdir(exist_ok=True); (WORK / "bin").mkdir(exist_ok=True)
    mint = None if args.no_mint else probe_mint()
    if any(CELLS[c]["kind"] == "mint" for c in cells):
        if mint:
            print(f"mint oracle: {mint['host']} lima/{mint['instance']}")
        else:
            print("mint oracle unavailable — running M5-local cells only")
            cells = [c for c in cells if CELLS[c]["kind"] != "mint"]
    print(f"building {len(tests)} test(s) + supervisor: {', '.join(tests)}")
    binmap = {t: build_variants(t) for t in tests}
    build_supervisor()
    fakefs = build_fakefs(tests)
    if mint and any(CELLS[c]["kind"] == "mint" for c in cells):
        push_to_mint(tests, binmap, mint)

    per_cell = {}
    for cell in cells:
        if CELLS[cell]["kind"] == "ish":
            res, dangling, secs = run_supervised(cell, tests, binmap, fakefs, args.seed)
            per_cell[cell] = res
            print(f"  supervised {cell:16} {len(tests)} tests {secs:6.2f}s  "
                  + (f"CRASH→{dangling}" if dangling else "clean"))
        else:
            per_cell[cell] = {t: run_cell(cell, t, binmap[t], fakefs, mint=mint,
                                          extra=["--seed", str(args.seed)]) for t in tests}

    any_fail = False
    for test in tests:
        results = [per_cell[c][test] for c in cells]
        mism, _ = compare(results)
        report(test, results, mism)
        if mism or any(r.status in ("CRASH", "HANG") for r in results):
            any_fail = True
    sys.exit(1 if any_fail else 0)

def cmd_device(args):
    """Run the corpus on a real device over ssh (device cells) with crash
    recovery, comparing against the host x86 oracle (Rosetta + mint). UNVALIDATED
    scaffold -- use --dry-run to inspect the ssh/scp/devicectl commands."""
    cfg = device_cfg(args)
    if not cfg["host"]:
        raise SystemExit("device backend needs --device-host (the device's ssh IP)")
    tests = args.tests.split(",") if args.tests else discover_tests()
    cells = args.cells.split(",") if args.cells else \
        [c for c, s in CELLS.items() if s["kind"] == "device"]
    WORK.mkdir(exist_ok=True); (WORK / "bin").mkdir(exist_ok=True)
    mint = None if args.no_mint else probe_mint()
    print(f"building {len(tests)} test(s) + supervisor: {', '.join(tests)}")
    binmap = {t: build_variants(t) for t in tests}
    build_supervisor()
    if mint:
        push_to_mint(tests, binmap, mint)
    print(f"deploying to {cfg['user']}@{cfg['host']}:{cfg['port']} {cfg['dir']}"
          + ("  (dry run)" if cfg["dry"] else ""))
    device_deploy(cfg, tests, binmap)

    per_cell = {}
    for cell in cells:
        print(f"device cell {cell}  (recover={cfg['recover']})")
        per_cell[cell] = device_run_batch(cfg, cell, tests, args.seed)
    if cfg["dry"]:
        print("\n(dry run — commands shown above; no device executed)")
        return
    oracle_cells = ["oracle"] + (["mint:x86_64", "mint:i386"] if mint else [])
    for cell in oracle_cells:
        per_cell[cell] = {t: run_cell(cell, t, binmap[t], None, mint=mint,
                                      extra=["--seed", str(args.seed)]) for t in tests}

    any_fail = False
    for test in tests:
        results = [per_cell[c][test] for c in per_cell]
        mism, _ = compare(results)
        report(test, results, mism)
        if mism or any(r.status in ("CRASH", "HANG") for r in results):
            any_fail = True
    sys.exit(1 if any_fail else 0)

def cmd_conform(args):
    """Signal/syscall conformance vs REAL LINUX. Runs each conformance test
    under every iSH cell and under mint's Linux VM (the oracle), then key-based
    compares. A divergence between an iSH cell and mint is a candidate Linux
    nonconformance (confirm against the man page before fixing — see the
    cmpxchg lesson; some behavior is genuinely unspecified)."""
    tests = args.tests.split(",") if args.tests else discover_conform()
    default = [c for c, s in CELLS.items() if s["kind"] in ("ish", "mint")]
    cells = args.cells.split(",") if args.cells else default
    WORK.mkdir(exist_ok=True); (WORK / "bin").mkdir(exist_ok=True)

    mint = None if args.no_mint else probe_mint()
    if any(CELLS[c]["kind"] == "mint" for c in cells):
        if mint:
            print(f"oracle = mint real Linux: {mint['host']} lima/{mint['instance']}")
        else:
            print("mint unavailable — running iSH cells only "
                  "(self-consistency across arch/engine, NOT real-Linux conformance)")
            cells = [c for c in cells if CELLS[c]["kind"] != "mint"]

    print(f"building {len(tests)} conformance test(s): {', '.join(tests)}")
    binmap = {t: build_conform_variants(t) for t in tests}
    fakefs = build_conform_fakefs(tests)
    if mint and any(CELLS[c]["kind"] == "mint" for c in cells):
        push_to_mint(tests, binmap, mint)

    summary, any_fail = {}, False
    for test in tests:
        results = [run_cell(c, test, binmap[test], fakefs, mint=mint,
                            extra=["--seed", str(args.seed)]) for c in cells]
        mism, _ = compare(results)
        report(test, results, mism)
        bad = [r.cell for r in results if r.status in ("CRASH", "HANG", "TEST_ERR")]
        if bad or mism:
            any_fail = True
        summary[test] = {
            "cells": {r.cell: {"status": r.status, "rc": r.rc,
                               "cases": len(parse(r.lines)), "secs": round(r.secs, 2)}
                      for r in results},
            "divergent_keys": len(mism),
            "bad_cells": bad,
        }
    (WORK / "conform-results.json").write_text(json.dumps(summary, indent=2))
    print(f"\nwrote {WORK / 'conform-results.json'}")
    sys.exit(1 if any_fail else 0)

def cmd_tier0(args):
    """Tier 0: build the tests/manual self-check suite and run it under iSH for
    each arch (i386 + amd64, default engine). Each test self-asserts '<name>:
    PASS' -- no oracle compare; this is the functional-regression pass gate."""
    tests = args.tests.split(",") if args.tests else discover_tier0()
    WORK.mkdir(exist_ok=True); (WORK / "bin").mkdir(exist_ok=True)
    print(f"building {len(tests)} self-check test(s) x {len(ZIG_TARGET)} arch(es)")
    built = build_tier0(tests)
    fakefs = {a: tier0_fakefs(a) for a in ZIG_TARGET}
    any_fail = False
    for arch in ZIG_TARGET:
        passed = failed = skipped = na = 0
        print(f"\n=== {arch} ===")
        for t in tests:
            if t not in built[arch]:
                na += 1
                continue
            verdict, rc, out = run_tier0(t, fakefs[arch])
            tail = (out.strip().splitlines() or [""])[-1][:90]
            if verdict == "PASS":
                passed += 1
            elif verdict == "SKIP":
                skipped += 1
                print(f"  [SKIP] {t:22} {tail}")
            else:
                failed += 1; any_fail = True
                print(f"  [{verdict:4}] {t:22} rc={rc}  {tail}")
        print(f"  {arch}: {passed} passed, {failed} failed, "
              f"{skipped} skipped, {na} n/a")
    sys.exit(1 if any_fail else 0)

def main():
    ap = argparse.ArgumentParser(description="iSH-AOK differential test conductor")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run"); r.set_defaults(fn=cmd_run)
    r.add_argument("--tests"); r.add_argument("--cells")
    r.add_argument("--seed", type=int, default=1)
    r.add_argument("--no-mint", action="store_true", help="skip the mint VM oracle cells")
    m = sub.add_parser("minimize"); m.set_defaults(fn=cmd_minimize)
    m.add_argument("--test", required=True); m.add_argument("--cell", required=True)
    m.add_argument("--seed", type=int, default=1)
    sp = sub.add_parser("supervise"); sp.set_defaults(fn=cmd_supervise)
    sp.add_argument("--tests"); sp.add_argument("--cells")
    sp.add_argument("--seed", type=int, default=1)
    sp.add_argument("--no-mint", action="store_true", help="skip the mint VM oracle cells")
    dv = sub.add_parser("device"); dv.set_defaults(fn=cmd_device)
    dv.add_argument("--tests"); dv.add_argument("--cells")
    dv.add_argument("--seed", type=int, default=1)
    dv.add_argument("--no-mint", action="store_true")
    dv.add_argument("--device-host", help="device ssh host/IP (varies per session)")
    dv.add_argument("--device-port", type=int, default=1022)
    dv.add_argument("--device-user", default="mke")
    dv.add_argument("--device-dir", default="/tmp/ish-remote")
    dv.add_argument("--recover", choices=["devicectl", "notify", "none"], default="notify")
    dv.add_argument("--device-udid", help="device UDID (for --recover devicectl)")
    dv.add_argument("--device-bundle", default="com.ish.iSH-AOK", help="bundle id (devicectl)")
    dv.add_argument("--dry-run", action="store_true",
                    help="print ssh/scp/devicectl commands without executing")
    cf = sub.add_parser("conform"); cf.set_defaults(fn=cmd_conform)
    cf.add_argument("--tests"); cf.add_argument("--cells")
    cf.add_argument("--seed", type=int, default=1)
    cf.add_argument("--no-mint", action="store_true", help="skip the mint oracle (self-consistency only)")
    t0 = sub.add_parser("tier0"); t0.set_defaults(fn=cmd_tier0)
    t0.add_argument("--tests", help="comma-separated subset (default: all self-check tests)")
    for tool in (ISH, FAKEFSIFY):
        if not tool.exists():
            raise SystemExit(f"missing {tool}; run `ninja -C build` first")
    args = ap.parse_args()
    args.fn(args)

if __name__ == "__main__":
    main()
