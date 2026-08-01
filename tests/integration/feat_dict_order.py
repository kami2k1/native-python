d = {}
for i in range(50):
    d[i] = i * i
print(len(d), d[7], d[49])
# deletion preserves order of the rest
del d[10]
del d[20]
del d[0]
ks = []
for k in d:
    ks.append(k)
print(ks[:5], len(ks))
# re-insert after delete goes to the end (Python semantics)
d[10] = 999
last = list(d.keys())[-1]
print(last, d[10])
# string-keyed dict order
m = {}
for w in ["banana", "apple", "cherry", "apple", "date"]:
    m[w] = m.get(w, 0) + 1
print(m)
print(list(m.keys()))
print(list(m.values()))
# set insertion iteration (CPython set order is hash-based, so sort for determinism)
s = set()
for x in [5, 3, 5, 1, 3, 9]:
    s.add(x)
print(sorted(s), len(s))
# dict comprehension order
print({c: ord(c) for c in "hello"})
# update existing key keeps position
o = {"x": 1, "y": 2, "z": 3}
o["y"] = 22
print(o)
# large churn: memory bounded, order stable
big = {}
for i in range(1000):
    big[i] = i
for i in range(500):
    del big[i]
tot = 0
for k in big:
    tot += k
print(len(big), tot, list(big.keys())[0], list(big.keys())[-1])
