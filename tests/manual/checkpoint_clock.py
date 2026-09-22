# checkpoint_clock.py -- the reported case: Python's time.sleep across a
# suspend. Driven by checkpoint_clock.sh; run it as root, since it asks for
# the suspend itself.
#
# CPython 3.11+ implements time.sleep as clock_nanosleep(CLOCK_MONOTONIC,
# TIMER_ABSTIME), so the deadline lives in the process's memory and the image
# carries it. A restore that started CLOCK_MONOTONIC again near zero left that
# deadline "uptime at the save" away: a restored daemon on the iPad slept a
# minute late. This asks for the suspend 0.3 s into a 1 s sleep and, after the
# restore, reports how long after the machine came back the sleep ended --
# measured, like checkpoint_clock.c, by a thread watching the wall clock for
# the gap the stop leaves, so the clock under test never grades itself.
import select
import subprocess
import sys
import threading
import time

gap = [0.0, 0.0, 0.0]           # the largest wall-clock gap, and its two edges


def spin():
    last = time.time()
    while True:
        # NOT time.sleep: that is the very absolute CLOCK_MONOTONIC sleep under
        # test, and a witness that sleeps that way wakes as late as the thing
        # it is watching -- which made this pass on the broken build. select's
        # timeout is relative.
        select.select([], [], [], 0.005)
        now = time.time()
        if now - last > gap[0]:
            gap[:] = [now - last, last, now]
        last = now


threading.Thread(target=spin, daemon=True).start()
# Up to the uptime a broken restore would add to the deadline.
while time.monotonic() < 8:
    time.sleep(0.2)
asker = subprocess.Popen(["/bin/sh", "-c", "sleep 0.3; echo suspend > /proc/ish/checkpoint"])
print("PY-SLEEP monotonic %.3f" % time.monotonic(), flush=True)
# One second at a time until the machine has stopped and come back: the app
# does work of its own before it freezes, and on an iPad that outlasted a
# single 1 s sleep. The sleep the stop landed in is the one graded.
for _ in range(60):
    m0, r0 = time.monotonic(), time.time()
    time.sleep(1.0)
    m1, r1 = time.monotonic(), time.time()
    if r1 - r0 > 1.5:           # a 1 s sleep that took longer: it held the stop
        break
select.select([], [], [], 0.1)  # time for the witness to have seen it
asker.wait()
stop, before, after = gap
# Allowing the witness its 5 ms steps; the stop itself is seconds long.
inside = before >= r0 - 0.01 and after <= r1 + 0.05
print("PY-WOKE after-resume %.3f stop %.3f slept-monotonic %.3f inside %d"
      % (r1 - after, stop, m1 - m0, inside), flush=True)
# The sleep had at most 0.7 s left when the machine stopped; a restarted
# clock adds the saved uptime, 8 s or more here.
ok = inside and stop > 1.5 and r1 - after <= 2.0 and m1 - m0 >= 1.0
print("PY-RESULT", "OK" if ok else "FAIL", flush=True)
sys.exit(0 if ok else 1)
