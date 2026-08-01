print({x: x*x for x in range(5)})
print({x for x in [1, 2, 2, 3, 3, 3]})
print([x*y for x in range(3) for y in range(3)])
print([x for x in range(20) if x % 2 == 0 if x % 3 == 0])
m = {"a": 1, "b": 2, "c": 3}
print({v: k for k, v in m.items()})
print([[r, c] for r in range(2) for c in range(2)])
words = ["hi", "hello", "hey", "x"]
print({w: len(w) for w in words if len(w) > 1})
grid = [[1, 2], [3, 4]]
print([cell for row in grid for cell in row])
print(sorted({len(w) for w in words}))
# starred list
a = [1, 2, 3]
print([0, *a, 4, *a])
def f(x): return [*range(x), x]
print(f(3))
# non-literal default
Z = 10
def g(n=Z*2): return n
print(g(), g(5))
# tuple-target assignment
(p, q) = (7, 8)
print(p, q)
