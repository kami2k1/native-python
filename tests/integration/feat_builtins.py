# New builtins and methods (v0.2).
print(all([1, True, 3]), all([1, 0]), any([0, 0]), any([0, 2]))
print(bin(10), hex(255), oct(64), bin(-5))
print(list("abc"), len(tuple([1, 2])), dict(), list())
dm = divmod(17, 5)
print(dm[0], dm[1])
dm = divmod(-7, 3)
print(dm[0], dm[1])
print(sum([1, 2, 3]), sum([1.5, 2.5]), sum([1, 2], 10))
print(sorted([3, 1, 2]), sorted(["b", "a"]))
print(min([4, 2, 9]), max([4, 2, 9]), min(1, 2), max(1, 2, 3))
print(round(3.14159, 2), round(3.7), round(-3.7))
print("a,b,,c".split(","), "a b  c".split())
print("-".join(["x", "y", "z"]))
print("  pad  ".strip() + "|", "  pad  ".lstrip() + "|", "  pad  ".rstrip() + "|")
print("hello".replace("l", "L"), "aaa".count("a"), "hello".find("ll"), "hello".find("z"))
print("Hello".startswith("He"), "Hello".endswith("lo"), "abc".isalpha(), "12".isdigit())
print("mixed Case".title(), "hEy".capitalize(), "AB".isupper(), "ab".islower())
print("42".zfill(5), "-42".zfill(5))
xs = [5, 3, 8, 1]
xs.sort()
print(xs)
xs.reverse()
print(xs)
xs.insert(1, 99)
print(xs, xs.index(99), xs.count(1))
xs.remove(99)
print(xs)
ys = xs.copy()
ys.extend([100, 200])
print(xs, ys)
print(ys.pop(0), ys)
d = {"a": 1, "b": 2}
print(sorted(d.keys()), sorted(d.values()))
items = d.items()
total = 0
for k, v in items:
    total += v
print(total)
for i, ch in enumerate(["x", "y"]):
    print(i, ch)
for a2, b2 in zip([1, 2, 3], ["a", "b"]):
    print(a2, b2)
print(format(3.14159, ".1f"), format(42, "06d"), format("hi", ">4"))
