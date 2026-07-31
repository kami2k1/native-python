# New v0.2 syntax: f-strings, slices, comprehensions, ternary, chained
# comparisons, tuple assignment, bitwise operators, inline bodies.
name = "world"
n = 7
print(f"hello {name}!")
print(f"{n} squared is {n * n}")
print(f"{3.14159:.2f}|{42:6d}|{7:04d}|{255:x}|{'hi':>5}|{0.5:.0%}")
print(f"{n = }")

xs = [0, 1, 2, 3, 4, 5, 6, 7]
print(xs[2:5], xs[:3], xs[5:], xs[::2], xs[::-1], xs[-3:])
s = "hello world"
print(s[:5], s[6:], s[::-1])

evens = [x for x in range(10) if x % 2 == 0]
sqs = [x * x for x in evens]
print(evens, sqs)
pairs = [[i, c] for i, c in enumerate("abc")]
print(pairs)
print(sum(x for x in range(10)))

a, b = 3, 4
a, b = b, a
print(a, b)
x = y = 9
print(x, y)
lo, hi = [1, 99]
print(lo, hi)

print(1 < 2 < 3, 3 > 2 > 1, 1 <= 1 < 2, 5 < 4 < 10)
print("big" if n > 5 else "small")
print(5 & 3, 5 | 3, 5 ^ 3, ~5, 1 << 8, 256 >> 4)
v = 6
v |= 1
v <<= 2
v &= 30
print(v)
print(2**10, 2**0.5 > 1.41, (-2)**2)

if n > 0: print("inline body works")
total = 0
for i in range(3): total += i
print(total)
