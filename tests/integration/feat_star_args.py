def f(a, b=2, *args, c=30, **kw):
    return [a, b, c, args, kw]

print(f(1))
print(f(1, 5, 9, 9, c=7, x=1))

def g(*args):
    total = 0
    for v in args:
        total += v
    return total

vals = [4, 5, 6]
print(g(1, *vals, 10))

def h(x, y, z):
    return x * 100 + y * 10 + z

d = {"y": 8, "z": 9}
print(h(7, **d))

add = lambda a, b=10, *rest: a + b + len(rest)
print(add(1), add(1, 2, 3, 4))

def kwonly(*, name="anon", age):
    return name + ":" + str(age)
print(kwonly(age=5), kwonly(name="bob", age=7))

def wrapper(fn, *args, **kwargs):
    return fn(*args, **kwargs)
print(wrapper(h, 1, 2, z=3))

class P:
    def __init__(self, x, *rest, **kw):
        self.x = x
        self.n = len(rest) + len(kw)
p = P(1, 2, 3, k=4)
print(p.x, p.n)
