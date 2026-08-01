# Generators: yield / yield from / send / close / next(), lazy for-loops,
# builtins over generators, generator methods, GC of abandoned generators.

def counter(n):
    i = 0
    while i < n:
        yield i
        i += 1

for x in counter(3):
    print(x)

g = counter(10)
print(next(g), next(g), next(g))

print(list(counter(4)))
print(sum(counter(100)))
print(max(counter(5)), min(counter(5)))
print(sorted(counter(3), reverse=True))

# bare yield + send
def echo():
    total = 0
    while True:
        v = yield total
        if v is not None:
            total += v

e = echo()
print(next(e))
print(e.send(5))
print(e.send(7))
e.close()

# yield from (generators and plain iterables)
def inner():
    yield 1
    yield 2

def outer():
    yield 0
    yield from inner()
    yield from [10, 20]
    yield 99

print(list(outer()))

# next() default + StopIteration is catchable
g2 = inner()
print(next(g2), next(g2), next(g2, "END"))
try:
    next(g2)
    print("unreachable")
except StopIteration:
    print("stopped")

# try/finally runs when a suspended generator is closed
log = []
def guarded():
    try:
        yield "a"
        yield "b"
    finally:
        log.append("cleanup")

fg = guarded()
print(next(fg))
fg.close()
print(log)

# exceptions inside a generator propagate to the consumer
def bad():
    yield 1
    raise ValueError("boom")

caught = "no"
try:
    for x in bad():
        pass
except ValueError:
    caught = "yes"
print("caught:", caught)

# infinite generator consumed lazily + nested generator pipelines
def naturals():
    n = 0
    while True:
        yield n
        n += 1

def squares(src):
    for v in src:
        yield v * v

total = 0
for v in squares(naturals()):
    if v > 1000:
        break
    total += v
print("total", total)

# generator methods on classes
class Tree:
    def __init__(self, vals):
        self.vals = vals

    def walk(self):
        for v in self.vals:
            yield v * 2

t = Tree([1, 2, 3])
print(list(t.walk()))

# closures over enclosing state can be generators too
def make_gen(base):
    def gen():
        yield base
        yield base + 1
    return gen

print(list(make_gen(40)()))

# abandoned generators are reclaimed by the GC (fiber stacks freed)
for i in range(3000):
    z = naturals()
    next(z)
print("gc ok")
