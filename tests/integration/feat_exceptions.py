# Exceptions: try/except/finally/else, raise, error propagation through calls,
# break/continue/return crossing try boundaries. (Not CPython-cross-checked:
# KamiPython error strings include the error-type prefix.)
def divide(a, b):
    if b == 0:
        raise ZeroDivisionError("division by zero")
    return a // b

try:
    print(divide(10, 2))
    print(divide(1, 0))
    print("unreachable")
except ZeroDivisionError as e:
    print("caught:", e)
finally:
    print("finally 1")

# else clause
try:
    v = divide(9, 3)
except Exception:
    print("no")
else:
    print("else ran:", v)

# runtime errors are catchable
try:
    xs = [1, 2]
    print(xs[10])
except IndexError:
    print("index error caught")

try:
    d = {}
    print(d["missing"])
except KeyError as e:
    print("key error caught")

# nested try + re-raise
def risky(n):
    try:
        if n == 1:
            raise ValueError("one")
        return 100
    except ValueError:
        raise

try:
    risky(1)
except ValueError:
    print("re-raised ok")
print(risky(0))

# return inside try (+ finally still runs)
def f():
    try:
        return "from try"
    finally:
        print("finally in f")

print(f())

# break/continue crossing a try boundary
total = 0
for i in range(10):
    try:
        if i == 3:
            continue
        if i == 6:
            break
        total += i
    finally:
        pass
print(total)  # 0+1+2+4+5 = 12

# try/finally without except propagates after running finally
def g():
    try:
        raise RuntimeError("boom")
    finally:
        print("finally in g")

try:
    g()
except RuntimeError as e:
    print("propagated:", e)

# assert
try:
    assert 1 + 1 == 3, "math is broken"
except AssertionError as e:
    print("assert caught:", e)
assert True
print("done")
