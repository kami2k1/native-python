with open("/tmp/kami_test.txt", "w") as f:
    f.write("line1\n")
    f.write("line2\n")
    f.write("line3\n")

with open("/tmp/kami_test.txt") as f:
    data = f.read()
print("len:", len(data))

lines = []
with open("/tmp/kami_test.txt") as f:
    for line in f:
        lines.append(line.strip())
print(lines)

# set
s = {1, 2, 3, 2, 1}
print(sorted(s), len(s))
s.add(4)
s.discard(1)
print(sorted(s), 2 in s, 9 in s)
a = {1, 2, 3}
b = {2, 3, 4}
print(sorted(a & b), sorted(a | b), sorted(a - b), sorted(a ^ b))
print(sorted(set([1, 1, 2, 3, 3])))

# del
d = {"a": 1, "b": 2, "c": 3}
del d["b"]
print(sorted(d.keys()))
xs = [10, 20, 30, 40]
del xs[1]
print(xs)

# sorted with key + reverse
words = ["ccc", "a", "bb"]
print(sorted(words, key=len))
print(sorted([3, 1, 2], reverse=True))
print(sorted(words, key=len, reverse=True))

# iterate dict directly
tot = 0
for k in {"x": 5, "y": 7}:
    tot += len(k)
print(tot)
