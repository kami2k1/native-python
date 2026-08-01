class Calc:
    def __init__(self, base=0):
        self.base = base
    def add(self, x, y, z=100, **opts):
        return self.base + x + y + z + len(opts)
    def collect(self, *items):
        return len(items)

c = Calc(1000)
args = [1, 2]
print(c.add(*args))
print(c.add(*args, z=7))
kw = {"z": 3, "extra": True}
print(c.add(1, 2, **kw))
print(c.collect(*[1, 2, 3], 4))
print(Calc.add(c, *args, z=0))

def churn(n):
    total = 0
    tmp = []
    for i in range(n):
        tmp = [i, i * 2]
        total += tmp[1]
    return total
print(churn(100000))
