# Sequence repetition, extended stdlib members, more str/list/set methods,
# int(x, base), str.format(), for/while...else — verified byte-identical CPython.
import math
import random

# --- sequence repetition ---
print([0] * 3, 3 * [1, 2], "ab" * 3, "x" * 0)

# --- math additions ---
print(math.factorial(6), math.gcd(24, 36), math.isqrt(99))
print(round(math.log(8, 2), 1), round(math.degrees(math.pi), 1))

# --- str methods ---
print("Hello".casefold(), "abc".index("c"), "hi".center(6, "*"))
print("{0} and {1} and {0}".format("A", "B"))
print("{:.3f}".format(1.0 / 3.0), "{:04d}".format(7))
print("path/to/file".removeprefix("path/"), "name.txt".removesuffix(".txt"))
print("123".isnumeric(), "a1".isalnum())

# --- int/float parsing ---
print(int("ff", 16), int(" 1010 ", 2), int("  -5  "), float(" 2.5 "))

# --- set methods ---
a = {1, 2, 3, 4}
b = {3, 4, 5, 6}
print(sorted(a | b), sorted(a & b), sorted(a - b))
print(sorted(a.union(b)), sorted(a.intersection(b)))
print(a.isdisjoint({9}), a.issuperset({1, 2}))

# --- list.clear ---
xs = [1, 2, 3]
xs.clear()
print(xs)

# --- for/while ... else ---
for i in range(3):
    pass
else:
    print("for-else")

found = False
for x in [1, 2, 3]:
    if x == 5:
        found = True
        break
else:
    print("not found in loop")
