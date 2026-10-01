#!/usr/bin/env python3
"""Workspace save and restore, end to end, in the iOS Simulator.

Two things put Workspace windows back, and neither had a test:

  arrangement  The Desktops applet's Save, read back on every ordinary launch
               (and by its Restore): which windows are open, where, and on
               which Desktop. Terminals included -- as fresh shells, since an
               arrangement carries no processes.
  suspend      Suspend to disk, then the next launch resumes the guest and the
               layout filed beside the image: the same terminals, the same
               tabs, the scrollback, and each tab attached to the SAME live
               shell it had (its $$ is unchanged).

It drives the app through app/WorkspaceTestHooks.m, which is inert unless the
app is launched with ISH_WORKSPACE_TEST=1 -- this script does that.

Usage:
  tests/workspace/restore_test.py --app <path to iSH-AOK.app> [--rootfs <alpine aarch64 minirootfs tarball>]
                                  [--only arrangement|suspend] [--keep]

Build the app first (see the simulator recipe in docs or the memory notes):
  xcodebuild -project iSH-AOK.xcodeproj -scheme iSH-AOK \\
      -configuration Debug-ApplePleaseFixFB19282108 -sdk iphonesimulator \\
      ARCHS=arm64 ONLY_ACTIVE_ARCH=NO ENABLE_DEBUG_DYLIB=NO -derivedDataPath <dd> build

It makes (or reuses) a private simulator, "AOK workspace-restore-test", so it
never touches a simulator anyone else is using. Exit status 0 when every
check passed; each failure is printed.
"""

import argparse
import json
import os
import plistlib
import subprocess
import sys
import tempfile
import time

BUNDLE = "app.ish.iSH-AOK"
SIM_NAME = "AOK workspace-restore-test"
DEVICE_TYPE = "com.apple.CoreSimulator.SimDeviceType.iPad-Pro-13-inch-M5-12GB"
NOTIFY = "app.ish.iSH-AOK.workspace-test"
ROOT_NAME = "wstest"
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def run(*cmd, check=True, capture=True, env=None):
    result = subprocess.run(cmd, check=False, text=True,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.STDOUT if capture else None, env=env)
    if check and result.returncode != 0:
        raise RuntimeError("%s failed (%d): %s" % (" ".join(cmd), result.returncode, result.stdout))
    return result.stdout or ""


def simctl(*args, **kw):
    return run("xcrun", "simctl", *args, **kw)


class Failures:
    def __init__(self):
        self.items = []

    def check(self, condition, message):
        if condition:
            print("  ok    " + message)
        else:
            print("  FAIL  " + message)
            self.items.append(message)
        return condition


class Sim:
    def __init__(self, app, rootfs):
        self.app = app
        self.rootfs = rootfs
        self.udid = None
        self.seq = 0

    # ---- set-up ----------------------------------------------------------

    def ensure_device(self):
        listing = json.loads(simctl("list", "devices", "-j"))
        for devices in listing["devices"].values():
            for device in devices:
                if device["name"] == SIM_NAME and device.get("isAvailable", True):
                    self.udid = device["udid"]
        if self.udid is None:
            runtimes = json.loads(simctl("list", "runtimes", "-j"))["runtimes"]
            ios = [r for r in runtimes if r.get("platform") == "iOS" and r.get("isAvailable")]
            if not ios:
                raise RuntimeError("no iOS simulator runtime")
            self.udid = simctl("create", SIM_NAME, DEVICE_TYPE, ios[-1]["identifier"]).strip()
        state = simctl("list", "devices", self.udid)
        if "(Booted)" not in state:
            simctl("boot", self.udid)
        simctl("bootstatus", self.udid, "-b")

    def install(self):
        # A simulator build carries empty entitlements; without the App Group
        # there is no root container and the guest never boots.
        with tempfile.NamedTemporaryFile("wb", suffix=".plist", delete=False) as f:
            plistlib.dump({"com.apple.security.application-groups": ["group.app.ish.iSH-AOK"]}, f)
            entitlements = f.name
        run("codesign", "--force", "--sign", "-", "--entitlements", entitlements, "--timestamp=none", self.app)
        os.unlink(entitlements)
        simctl("install", self.udid, self.app)
        # One "<group id>\t<path>" line per App Group.
        group_dir = None
        for line in simctl("get_app_container", self.udid, BUNDLE, "groups").splitlines():
            if line.startswith("group.app.ish.iSH-AOK"):
                group_dir = line.split("\t", 1)[-1].strip()
        if group_dir is None:
            raise RuntimeError("no group container after install")
        self.group_dir = group_dir
        root = os.path.join(group_dir, "roots", ROOT_NAME)
        if not os.path.isdir(os.path.join(root, "data")):
            os.makedirs(os.path.dirname(root), exist_ok=True)
            run(os.path.join(REPO, "build", "tools", "fakefsify"), self.rootfs, root)
        self.defaults("write", "Default Root", "-string", ROOT_NAME)
        self.defaults("write", "Suspend To Disk", "-bool", "YES")

    def forget_sessions(self):
        """No saved session and no saved arrangement: a launch boots fresh,
        without asking, and opens nothing an earlier run left behind."""
        self.terminate()
        for key in ("ISHWorkspaceSavedDesktops", "ISHWorkspaceSavedLayout", "ISHWorkspaceSavedLayoutTimes"):
            self.defaults("delete", key)
        sessions = os.path.join(self.group_dir, "sessions")
        if os.path.isdir(sessions):
            for name in os.listdir(sessions):
                os.unlink(os.path.join(sessions, name))

    def defaults(self, verb, key, *value):
        # By the app's own plist, as a path domain: `defaults ... app.ish.iSH-AOK`
        # from simctl spawn reads and writes some other file, so a delete there
        # removed nothing and a read said every key was absent.
        domain = os.path.join(self.data_dir(), "Library", "Preferences", BUNDLE)
        simctl("spawn", self.udid, "defaults", verb, domain, key, *value, check=verb != "delete")

    def data_dir(self):
        return simctl("get_app_container", self.udid, BUNDLE, "data").strip()

    def test_dir(self):
        return os.path.join(self.data_dir(), "tmp", "workspace-test")

    # ---- the app ---------------------------------------------------------

    def terminate(self):
        simctl("terminate", self.udid, BUNDLE, check=False)
        time.sleep(1)

    def launch(self):
        self.terminate()
        ready = os.path.join(self.test_dir(), "ready")
        if os.path.exists(ready):
            os.unlink(ready)
        env = dict(os.environ, SIMCTL_CHILD_ISH_WORKSPACE_TEST="1")
        simctl("launch", self.udid, BUNDLE, env=env)
        self.wait(lambda: os.path.exists(ready), 60, "the app to start")
        self.seq = 0

    def running(self):
        return BUNDLE in simctl("spawn", self.udid, "launchctl", "list", check=False)

    def command(self, *words, timeout=30, wait=True):
        self.seq += 1
        seq = "%d-%d" % (int(time.time()), self.seq)
        directory = self.test_dir()
        result = os.path.join(directory, "result-%s.json" % seq)
        with open(os.path.join(directory, "cmd.txt"), "w") as f:
            f.write("%s %s\n" % (seq, " ".join(str(w) for w in words)))
        simctl("spawn", self.udid, "notifyutil", "-p", NOTIFY)
        if not wait:
            return None
        self.wait(lambda: os.path.exists(result), timeout, "an answer to %s" % words[0])
        with open(result) as f:
            answer = json.load(f)
        os.unlink(result)
        if not answer.get("ok"):
            raise RuntimeError("%s: %s" % (" ".join(str(w) for w in words), answer))
        return answer

    def dump(self):
        return self.command("dump")

    @staticmethod
    def wait(predicate, timeout, what):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                if predicate():
                    return
            except Exception:
                pass
            time.sleep(0.5)
        raise RuntimeError("timed out waiting for " + what)

    # ---- helpers on a dump ----------------------------------------------

    @staticmethod
    def terminals(dump):
        return [w for w in dump.get("windows", []) if w.get("kind") == "terminal"]

    def wait_for_terminals(self, count, timeout=90):
        holder = {}

        def ready():
            d = self.dump()
            terms = self.terminals(d)
            if len(terms) >= count and all(t.get("tabs") for t in terms):
                holder["dump"] = d
                return True
            return False
        self.wait(ready, timeout, "%d terminal window(s)" % count)
        return holder["dump"]

    def wait_for_text(self, window_id, tab, text, timeout=60):
        def shows():
            for w in self.terminals(self.dump()):
                if w["id"] == window_id and tab < len(w["tabs"]):
                    return text in w["tabs"][tab].get("contents", "")
            return False
        self.wait(shows, timeout, "%r in window %s tab %d" % (text, window_id, tab))

    def enter_workspace(self):
        if not self.dump().get("workspace"):
            self.command("workspace")
            self.wait(lambda: self.dump().get("workspace"), 30, "the Workspace")


def frame_close(a, b, slack=0.02):
    return all(abs(a[k] - b[k]) <= slack for k in ("x", "y", "width", "height"))


def window_with(dump, marker):
    """The terminal window one of whose tabs shows marker, and that tab."""
    for w in Sim.terminals(dump):
        for index, tab in enumerate(w["tabs"]):
            if marker in tab.get("contents", ""):
                return w, index
    return None, None


def build_scenario(sim, tag, extra=0):
    """Two terminals: A alone on Desktop 2, B with two tabs on Desktop 1; and
    `extra` more two-tab windows, C1.., on Desktop 1 where they open.

    Returns {name: (window id, tab, shell pid)} and the dump."""
    sim.enter_workspace()
    # Anything already open stays; the markers tell ours apart.
    before = {w["id"] for w in sim.terminals(sim.dump())}
    sim.command("open-terminal")
    sim.command("open-terminal")
    d = sim.wait_for_terminals(len(before) + 2)
    new = [w for w in sim.terminals(d) if w["id"] not in before]
    a, b = new[0]["id"], new[1]["id"]
    sim.command("new-tab", b)
    sim.wait(lambda: len([w for w in sim.terminals(sim.dump()) if w["id"] == b][0]["tabs"]) == 2,
             30, "B's second tab")
    sim.command("desktops", 2)
    sim.command("frame", a, 0.05, 0.10, 0.40, 0.45)
    sim.command("frame", b, 0.50, 0.30, 0.45, 0.50)
    sim.command("assign", a, 1)
    sim.command("assign", b, 0)
    sim.command("switch", 0)
    tabs = {"A": (a, 0), "B0": (b, 0), "B1": (b, 1)}
    for n in range(1, extra + 1):
        known = {w["id"] for w in sim.terminals(sim.dump())}
        sim.command("open-terminal")
        d = sim.wait_for_terminals(len(known) + 1)
        c = [w for w in sim.terminals(d) if w["id"] not in known][0]["id"]
        sim.command("new-tab", c)
        sim.wait(lambda: len([w for w in sim.terminals(sim.dump()) if w["id"] == c][0]["tabs"]) == 2,
                 30, "C%d's second tab" % n)
        tabs["C%d-0" % n] = (c, 0)
        tabs["C%d-1" % n] = (c, 1)
    shells = {}
    for name, (window, tab) in tabs.items():
        marker = "%s-%s" % (tag, name)
        # The shell prints its own pid after the marker: the pid is what a
        # resume has to give back, and the marker finds the window again.
        sim.command("type", window, tab, "echo %s=$$\\r" % marker)
        sim.wait_for_text(window, tab, marker + "=", 60)
    d = sim.dump()
    for name in tabs:
        marker = "%s-%s=" % (tag, name)
        w, index = window_with(d, marker)
        contents = w["tabs"][index]["contents"]
        line = [l for l in contents.splitlines() if l.startswith(marker)][-1]
        shells[name] = (w["id"], index, line[len(marker):].strip())
    return shells, d


def snapshot(dump, shells):
    """What has to come back: per window, its frame, Desktop and tab count."""
    out = {}
    for name, (window_id, tab, pid) in shells.items():
        w = [x for x in Sim.terminals(dump) if x["id"] == window_id][0]
        out[name] = {"frame": w["frame"], "desktop": w["desktop"], "tabs": len(w["tabs"]),
                     "tab": tab, "pid": pid}
    return out


def test_arrangement(sim, f):
    print("arrangement: Save, relaunch, everything back as fresh shells")
    sim.forget_sessions()
    sim.launch()
    shells, d = build_scenario(sim, "ARR")
    want = snapshot(d, shells)
    sim.command("save-arrangement")
    sim.launch()
    sim.enter_workspace()
    try:
        d = sim.wait_for_terminals(2, timeout=60)
    except RuntimeError:
        d = sim.dump()
    terms = sim.terminals(d)
    f.check(len(terms) == 2, "both terminal windows back, and no others (found %d)" % len(terms))
    by_tabs = sorted(terms, key=lambda w: len(w["tabs"]))
    if len(terms) >= 2:
        single, double = by_tabs[0], by_tabs[-1]
        f.check(len(double["tabs"]) == 2, "B has its two tabs (%d)" % len(double["tabs"]))
        f.check(frame_close(single["frame"], want["A"]["frame"]),
                "A where it was: %s vs %s" % (single["frame"], want["A"]["frame"]))
        f.check(frame_close(double["frame"], want["B0"]["frame"]),
                "B where it was: %s vs %s" % (double["frame"], want["B0"]["frame"]))
        f.check(single["desktop"] == want["A"]["desktop"],
                "A on Desktop %d (%d)" % (want["A"]["desktop"] + 1, single["desktop"] + 1))
        f.check(double["desktop"] == want["B0"]["desktop"],
                "B on Desktop %d (%d)" % (want["B0"]["desktop"] + 1, double["desktop"] + 1))
    f.check(d.get("desktopCount", 0) >= 2, "two Desktops (%s)" % d.get("desktopCount"))


def test_suspend(sim, f):
    # Nine sessions: the restore used to publish only eight, and the rest came
    # back as fresh shells under the old scrollback.
    print("suspend: suspend to disk, relaunch, the same live shells back (9 sessions)")
    sim.forget_sessions()
    sim.launch()
    shells, d = build_scenario(sim, "SUS", extra=3)
    want = snapshot(d, shells)
    sim.command("suspend-exit", wait=False)
    sim.wait(lambda: not sim.running(), 120, "the suspend to exit the app")
    sim.launch()
    # The launch asks which saved session to resume, then whether to keep the
    # image: the first, then delete it, so the next run starts clean.
    sim.wait(lambda: sim.command("tap", "localhost") is not None, 60, "the resume picker")
    sim.wait(lambda: sim.command("tap", "Resume and Delete") is not None, 30, "the resume choice")
    sim.wait(lambda: sim.dump().get("restored"), 120, "the guest to be resumed")
    sim.enter_workspace()
    try:
        d = sim.wait_for_terminals(2, timeout=90)
    except RuntimeError:
        d = sim.dump()
    f.check(d.get("restored"), "the session was resumed, not booted fresh")
    for name, expect in want.items():
        marker = "SUS-%s=%s" % (name, expect["pid"])
        w, index = window_with(d, marker)
        if not f.check(w is not None, "%s's scrollback came back (%s)" % (name, marker)):
            continue
        f.check(len(w["tabs"]) == expect["tabs"], "%s's window has %d tab(s) (%d)" % (name, expect["tabs"], len(w["tabs"])))
        f.check(frame_close(w["frame"], expect["frame"]), "%s where it was: %s vs %s" % (name, w["frame"], expect["frame"]))
        f.check(w["desktop"] == expect["desktop"], "%s on Desktop %d (%d)" % (name, expect["desktop"] + 1, w["desktop"] + 1))
        # Live, and the same shell: ask it for its pid again.
        probe = "SUS-%s-AFTER=" % name
        sim.command("type", w["id"], index, "echo %s$$\\r" % probe)
        try:
            sim.wait_for_text(w["id"], index, probe, 30)
            contents = [x for x in sim.terminals(sim.dump()) if x["id"] == w["id"]][0]["tabs"][index]["contents"]
            line = [l for l in contents.splitlines() if l.startswith(probe)][-1]
            pid = line[len(probe):].strip()
            f.check(pid == expect["pid"], "%s is the same shell, pid %s (%s)" % (name, expect["pid"], pid))
        except RuntimeError:
            f.check(False, "%s's shell answered after the resume" % name)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True)
    parser.add_argument("--rootfs", default=os.path.join(REPO, "alpine-minirootfs-3.24.2-aarch64.tar.xz"))
    parser.add_argument("--only", choices=("arrangement", "suspend"))
    parser.add_argument("--keep", action="store_true", help="leave the app running at the end")
    args = parser.parse_args()

    sim = Sim(os.path.abspath(args.app), os.path.abspath(args.rootfs))
    sim.ensure_device()
    sim.install()
    f = Failures()
    tests = {"arrangement": test_arrangement, "suspend": test_suspend}
    for name, test in tests.items():
        if args.only and args.only != name:
            continue
        try:
            test(sim, f)
        except RuntimeError as error:
            f.check(False, "%s: %s" % (name, error))
    if not args.keep:
        sim.terminate()
    print()
    if f.items:
        print("%d check(s) FAILED" % len(f.items))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
