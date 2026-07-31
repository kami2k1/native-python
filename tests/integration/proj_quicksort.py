# Real project: quicksort of 20000 pseudo-random ints (deterministic LCG),
# verified sorted + order-independent checksum.
def lcg_list(n, seed):
    xs = []
    x = seed
    for i in range(n):
        x = (1103515245 * x + 12345) % 2147483648
        xs.append(x % 100000)
    return xs

def quicksort(xs, lo, hi):
    if lo >= hi:
        return None
    p = xs[(lo + hi) // 2]
    i = lo
    j = hi
    while i <= j:
        while xs[i] < p:
            i += 1
        while xs[j] > p:
            j -= 1
        if i <= j:
            tmp = xs[i]
            xs[i] = xs[j]
            xs[j] = tmp
            i += 1
            j -= 1
    quicksort(xs, lo, j)
    quicksort(xs, i, hi)
    return None

data = lcg_list(20000, 42)
quicksort(data, 0, len(data) - 1)

ok = True
for i in range(1, len(data)):
    if data[i - 1] > data[i]:
        ok = False
print(ok)

checksum = 0
for i in range(len(data)):
    checksum = (checksum * 31 + data[i]) % 1000000007
print(checksum)
print(data[0], data[10000], data[19999])
