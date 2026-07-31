xs = [1, 2, 3]
xs.append(4)
print(xs)
print(len(xs))
xs[0] = 10
print(xs[0], xs[-1])
print(xs.pop())
print(xs)
total = 0
for v in xs:
    total += v
print(total)
ys = xs + [7, 8]
print(ys)
print([1, 2] == [1, 2])
print(2 in ys, 99 in ys)
m = [[1, 2], [3, 4]]
print(m[1][0])
