d = {"a": 1, "b": 2}
d["c"] = 3
print(d["a"] + d["b"] + d["c"])
print("a" in d, "z" in d)
print(d.get("z", 99))
print(len(d))
ks = d.keys()
total = 0
for k in ks:
    total += d[k]
print(total)
counts = {}
for w in ["x", "y", "x"]:
    counts[w] = counts.get(w, 0) + 1
print(counts["x"], counts["y"])
print({1: "one"}[1])
