#!/usr/bin/env python3
"""Job control in a native shell, driven through a real pty.

    python3 tests/remote/native_job_control.py <fakefs-root> [/AOK/native/bash]

On a device, where the shell is reached over ssh rather than through the CLI,
give the launch command instead -- the pty is this end's either way:

    AOK_SHELL_CMD="ssh -tt -p 1022 mke@<ip> exec /AOK/native/bash -i" \
        python3 tests/remote/native_job_control.py

WHY A PTY, when tests/manual already has two native-shell suites. Those run a
shell with -c and check what it prints, which covers everything about a shell
EXCEPT the part that needs a terminal: ^C reaching a foreground job, ^Z and fg,
a job table, a trap firing while the shell waits. All of that goes through the
shim's signal path, and none of it is reachable from a script.

The class it guards, from the change that prompted it: the shim's handler table
used to be per THREAD, and the held set was computed one registration behind.
Neither shows in a shell -- a shell installs a dozen handlers and the next
registration papers over the last -- and both were fatal for a program with
exactly one. A script test cannot tell the difference; this can.

Exit status is 0 only if every case passes.
"""
import os, pty, select, signal, sys, time, re

import shlex
CMD = shlex.split(os.environ.get("AOK_SHELL_CMD", ""))
if not CMD and len(sys.argv) < 2:
    sys.exit(__doc__)
ROOT = sys.argv[1] if len(sys.argv) > 1 else ""
SHELL = sys.argv[2] if len(sys.argv) > 2 else "/AOK/native/bash"
ISH = os.environ.get("ISH_BIN", "./build-rust/ish")
MARK = "AOKPROMPT>"

def spawn():
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["PS1"] = MARK
        os.environ["PROMPT"] = MARK
        os.environ["TERM"] = "dumb"
        if CMD:
            os.execvp(CMD[0], CMD)
        # Through /bin/sh: the CLI cannot exec a /AOK/native entry directly,
        # since those are dispatch names rather than files on the rootfs.
        os.execv(ISH, [ISH, "-f", ROOT, "/bin/sh", "-c", "exec " + SHELL + " -i"])
    return pid, fd

# A real prompt on a real device is not plain text: it carries OSC title and
# colour-query sequences, and a themed zsh emits a run of them before every
# prompt. Matching against the raw stream made the harness report "shell never
# prompted" at a shell that was plainly prompting.
ANSI = re.compile(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[\[\]P][0-?]*[ -/]*[@-~]|\x1b.")

def strip_ansi(text):
    return ANSI.sub("", text)

def read_until(fd, pattern, timeout=20):
    buf = ""
    end = time.time() + timeout
    rx = re.compile(pattern)
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.3)
        if not r:
            continue
        try:
            data = os.read(fd, 65536)
        except OSError:
            break
        if not data:
            break
        buf += data.decode("utf-8", "replace")
        # A terminal query left unanswered stalls zsh's zle before it draws a
        # prompt, so the two it actually waits on are answered here: primary
        # device attributes, and the cursor position report.
        if "\x1b[c" in buf or "\x1b[0c" in buf:
            os.write(fd, b"\x1b[?1;2c")
        if "\x1b[6n" in buf:
            os.write(fd, b"\x1b[1;1R")
        if rx.search(strip_ansi(buf)):
            return True, strip_ansi(buf)
    return False, strip_ansi(buf)

def send(fd, s):
    os.write(fd, s.encode())
    time.sleep(0.15)

results = []
def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(("  PASS  " if ok else "  FAIL  ") + name + (("   " + detail) if detail and not ok else ""))

pid, fd = spawn()
# An interactive shell writes its OWN prompt whatever the environment said, so
# the marker is installed once it is up rather than inherited.
ok, out = read_until(fd, r"[$#>%] ?$|[$#>%]\s*$", timeout=20)
if not ok:
    print("  FAIL  shell never prompted\n---\n" + out[-800:])
    os.kill(pid, signal.SIGKILL); sys.exit(1)
send(fd, "PS1='%s '; PROMPT='%s '\n" % (MARK, MARK))
ok, out = read_until(fd, re.escape(MARK), timeout=15)
if not ok:
    print("  FAIL  marker prompt never appeared\n---\n" + out[-800:])
    os.kill(pid, signal.SIGKILL); sys.exit(1)
read_until(fd, re.escape(MARK), timeout=3)

# 1. ^C kills a foreground job and the shell survives with status 130.
send(fd, "sleep 30\n")
time.sleep(0.6)
send(fd, "\x03")                      # ^C
ok, out = read_until(fd, re.escape(MARK), timeout=10)
check("ctrl-c interrupts a foreground job", ok, out[-300:])
send(fd, "echo rc=$?\n")
ok, out = read_until(fd, r"rc=\d+", timeout=10)
m = re.search(r"rc=(\d+)", out or "")
check("ctrl-c yields status 130", bool(m) and m.group(1) == "130",
      "got " + (m.group(1) if m else "nothing"))

# 2. A background job runs, is listed, and can be killed by job spec.
read_until(fd, re.escape(MARK), timeout=5)
send(fd, "sleep 30 &\n")
ok, out = read_until(fd, re.escape(MARK), timeout=10)
send(fd, "jobs\n")
ok, out = read_until(fd, r"sleep", timeout=10)
check("background job is listed by jobs", ok, out[-200:])
read_until(fd, re.escape(MARK), timeout=5)
send(fd, "kill %1\n")
time.sleep(0.5)
send(fd, "echo killed=$?\n")
ok, out = read_until(fd, r"killed=\d+", timeout=10)
m = re.search(r"killed=(\d+)", out or "")
check("kill %1 succeeds", bool(m) and m.group(1) == "0",
      "got " + (m.group(1) if m else "nothing"))

# 3. ^Z stops a job and fg resumes it to completion.
read_until(fd, re.escape(MARK), timeout=5)
send(fd, "sleep 2\n")
time.sleep(0.5)
send(fd, "\x1a")                      # ^Z
ok, out = read_until(fd, r"(?i)(stopped|suspend)", timeout=10)
check("ctrl-z stops a foreground job", ok, out[-300:])
read_until(fd, re.escape(MARK), timeout=5)
send(fd, "fg\n")
ok, out = read_until(fd, re.escape(MARK), timeout=15)
check("fg resumes it to completion", ok, out[-300:])

# 4. A trap still fires, and the shell keeps going afterwards.
#    Both are read from ONE capture: read_until drops what it has already
#    consumed, and "after" arrives in the same chunk as "TRAPPED".
send(fd, "trap 'echo TRAPPED' USR1; kill -USR1 $$; echo AFTERTRAP\n")
ok, out = read_until(fd, r"AFTERTRAP\r?\n", timeout=10)
check("trap fires on a self-signal", "TRAPPED" in out, out[-300:])
check("shell continues after the trap", ok, out[-300:])

# 5. A pipeline of external commands still reports the right status.
read_until(fd, re.escape(MARK), timeout=5)
send(fd, "/bin/echo hi | /bin/cat; echo pipe=$?\n")
ok, out = read_until(fd, r"pipe=\d+", timeout=10)
m = re.search(r"pipe=(\d+)", out or "")
check("pipeline of externals returns 0", bool(m) and m.group(1) == "0",
      "got " + (m.group(1) if m else "nothing"))

send(fd, "exit\n")
time.sleep(0.4)
try:
    os.kill(pid, signal.SIGKILL)
except OSError:
    pass
os.waitpid(pid, 0)

bad = [n for n, ok, _ in results if not ok]
print("  %d passed, %d failed" % (len(results) - len(bad), len(bad)))
sys.exit(1 if bad else 0)
