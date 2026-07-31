# GC stress: allocate millions of short-lived objects; memory must stay bounded.
def make_garbage(n):
    total = 0
    for i in range(n):
        tmp = [i, i + 1, i + 2, "abcdefghijklmnop"]
        d = {"k": tmp, "i": i}
        total += d["i"]
    return total

print(make_garbage(200000))
print("gc ok")
