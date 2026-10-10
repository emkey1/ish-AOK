# pybench.py -- interpreter-heavy microbenchmarks, min of 3 runs, in ms
import time
def fib(n): return n if n < 2 else fib(n-1) + fib(n-2)
class P:
    def __init__(s): s.v = 0
    def bump(s, d): s.v += d; return s.v
def b_fib(): fib(24)
def b_method():
    p = P()
    for i in range(300000): p.bump(i & 7)
def b_dict():
    d = {}
    for i in range(150000): d["k%d" % i] = i
    t = 0
    for i in range(150000): t += d["k%d" % i]
def b_str():
    parts = []
    for i in range(60000):
        s = "item-%05d" % i
        parts.append(s.upper().replace("ITEM", "x")[::-1])
    j = ",".join(parts); j.split(","); j.count("x")
out = []
for name, f in (("fib", b_fib), ("method", b_method), ("dict", b_dict), ("str", b_str)):
    best = None
    for _ in range(3):
        t = time.perf_counter(); f(); dt = (time.perf_counter() - t) * 1000
        best = dt if best is None or dt < best else best
    out.append("%s=%d" % (name, best))
print(" ".join(out))
