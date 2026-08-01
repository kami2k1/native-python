# KamiPython benchmark suite — the same file runs under CPython and kamipy.
# Prints "name,seconds" lines so run_benchmark.py can build a comparison table.
import time


def integer_loop(n):
    total = 0
    for i in range(n):
        total = (total + i * 3) % 1000000007
    return total


def math_test(n):
    acc = 0.0
    for i in range(n):
        acc = acc + i * 0.5 - acc / 3.0
    return acc


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def string_test(n):
    s = ""
    for i in range(n):
        s += "x"
    return len(s)


def bench(name, fn, arg):
    t0 = time.perf_counter()
    r = fn(arg)
    t1 = time.perf_counter()
    print(name + "," + str(t1 - t0) + "," + str(r))


bench("integer_loop", integer_loop, 100000000)
bench("math", math_test, 100000000)
bench("recursion", fib, 32)
bench("string_test", string_test, 1000000)
