#!/usr/bin/env python3
"""KamiPython benchmark runner.

Builds and runs the same benchmark program with:
  * kamipy   (compiled native binary)
  * Go       (reference "systems language" target), when `go` is installed
  * CPython  (baseline), when `python3` is installed

and prints a comparison table. Run from anywhere:

    python3 run_benchmark.py [--kamipy /path/to/kamipy]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def find_kamipy(explicit):
    if explicit:
        return explicit
    cands = [
        os.path.join(HERE, "kamipy"),
        os.path.join(HERE, "kamipy.exe"),
        os.path.join(HERE, "..", "build", "kamipy"),
        os.path.join(HERE, "build", "kamipy"),
        shutil.which("kamipy"),
    ]
    for c in cands:
        if c and os.path.exists(c):
            return os.path.abspath(c)
    sys.exit("cannot find the kamipy binary — pass --kamipy /path/to/kamipy")


def find_source(name):
    for d in (HERE, os.path.join(HERE, "benchmarks"), os.path.join(HERE, "..", "benchmarks")):
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    sys.exit("cannot find " + name + " next to this script")


def parse(output):
    res = {}
    for line in output.splitlines():
        parts = line.strip().split(",")
        if len(parts) >= 2:
            try:
                res[parts[0]] = float(parts[1])
            except ValueError:
                pass
    return res


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kamipy")
    args = ap.parse_args()

    kamipy = find_kamipy(args.kamipy)
    bench_py = find_source("bench.py")
    bench_go = find_source("bench.go")
    tmp = tempfile.mkdtemp(prefix="kamibench_")

    print("== building kamipy binary (embedded LLVM backend) ==")
    kami_bin = os.path.join(tmp, "bench_kami")
    run([kamipy, "build", bench_py, "-o", kami_bin])
    print("== running kamipy ==")
    kami = parse(run([kami_bin]))

    go = {}
    gobin = shutil.which("go")
    if gobin:
        print("== building + running Go ==")
        go_bin = os.path.join(tmp, "bench_go")
        env = dict(os.environ, GOFLAGS="-ldflags=-s -w")
        run([gobin, "build", "-o", go_bin, bench_go], env=env, cwd=tmp)
        go = parse(run([go_bin]))
    else:
        print("(go not installed — skipping the Go column)")

    cpy = {}
    py = shutil.which("python3") or shutil.which("python")
    if py:
        print("== running CPython (string_test uses CPython's own += fast path) ==")
        cpy = parse(run([py, bench_py]))
    else:
        print("(python3 not installed — skipping the CPython column)")

    names = ["integer_loop", "math", "recursion", "string_test"]
    print()
    hdr = f"{'benchmark':<14}{'kamipy':>12}{'go':>12}{'cpython':>12}{'vs go':>10}{'vs cpython':>13}"
    print(hdr)
    print("-" * len(hdr))
    for n in names:
        k = kami.get(n)
        g = go.get(n)
        c = cpy.get(n)
        f = lambda v: (f"{v:9.4f}s" if v is not None else "        —")

        def ratio(base):
            if k is None or base is None or k <= 0:
                return "        —"
            r = base / k
            return f"{r:9.2f}x"

        print(f"{n:<14}{f(k):>12}{f(g):>12}{f(c):>12}{ratio(g):>10}{ratio(c):>13}")
    print("\n('vs go' > 1.00x means kamipy is faster than Go on this machine)")


if __name__ == "__main__":
    main()
