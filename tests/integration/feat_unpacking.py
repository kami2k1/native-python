# starred list literals, tuple targets, generics syntax, non-literal defaults
a = [1, 2, 3]
print([0, *a, 4])
print([*a, *a])
def spread(x):
    return [*range(x), 99]
print(spread(3))
(p, q) = (10, 20)
print(p, q)
(x, y, z) = [1, 2, 3]
print(x + y + z)

DEFAULT = 100
def f(n=DEFAULT + 1, items=[]):
    return n + len(items)
print(f(), f(5), f(5, [1, 2]))

def make():
    return [x * 2 for x in range(4)]
print(make())
