print(list(map(lambda x: x * x, [1, 2, 3, 4])))
print(list(filter(lambda x: x % 2 == 0, range(10))))
data = [[1, "b"], [3, "a"], [2, "c"]]
print(sorted(data, key=lambda p: p[0]))
print(sorted(data, key=lambda p: p[1]))
print(sorted([5, -3, 1, -8], key=lambda x: -x))

# closure capturing enclosing locals
def make_adder(n):
    def add(x):
        return x + n
    return add

add5 = make_adder(5)
add10 = make_adder(10)
print(add5(1), add10(1), add5(100))

def multiplier(factor):
    return lambda x: x * factor

times3 = multiplier(3)
print(times3(4), list(map(multiplier(2), [1, 2, 3])))

# nested function using several captured vars
def outer(a, b):
    def inner(c):
        return a + b + c
    return inner(10)

print(outer(1, 2))

# decorator (module-level)
def shout(fn):
    def wrapper(x):
        return fn(x).upper()
    return wrapper

@shout
def greet(name):
    return "hi " + name

print(greet("kami"))

def tag(fn):
    def w(x):
        return "[" + fn(x) + "]"
    return w

@tag
@shout
def label(s):
    return s

print(label("ok"))

# map with a named function (first-class)
def sq(x):
    return x * x
print(list(map(sq, [1, 2, 3])))

# filter + lambda capturing
threshold = 3
print(list(filter(lambda x: x > threshold, [1, 2, 3, 4, 5])))

# try/except inside a closure that reads a captured variable (regression: the
# outlined try body did not receive the capture pointer and failed to compile)
def make_safe(fallback):
    def wrapper(fn, *args):
        try:
            return fn(*args)
        except Exception:
            return fallback
    return wrapper


def risky(x):
    if x == 0:
        raise ValueError("zero")
    return 100 // x


safe = make_safe("failed")
print(safe(risky, 4), safe(risky, 0))
